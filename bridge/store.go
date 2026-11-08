package main

import (
	"context"
	"database/sql"
	"errors"
	"strconv"
)

// Chat is one row of the chat list shown on the 3DS.
type Chat struct {
	JID     string
	Name    string
	IsGroup bool
	LastTS  int64
	Unread  int
	Preview string
}

func (c Chat) fields() []string {
	return []string{c.JID, c.Name, b01(c.IsGroup), strconv.FormatInt(c.LastTS, 10), strconv.Itoa(c.Unread), c.Preview}
}

// Msg is one stored message. Media holds the serialized waE2E.Message for
// messages whose image can be fetched later.
type Msg struct {
	Chat       string
	ID         string
	TS         int64
	FromMe     bool
	Sender     string
	SenderName string
	Kind       string
	Text       string
	Media      []byte
}

func (m Msg) fields() []string {
	return []string{m.Chat, m.ID, strconv.FormatInt(m.TS, 10), b01(m.FromMe), m.SenderName, m.Kind, b01(len(m.Media) > 0), m.Text}
}

func b01(b bool) string {
	if b {
		return "1"
	}
	return "0"
}

// Store keeps the bridge's own chat and message tables next to whatsmeow's tables.
type Store struct{ db *sql.DB }

const schema = `
CREATE TABLE IF NOT EXISTS wa3ds_chats (
	jid      TEXT PRIMARY KEY,
	name     TEXT NOT NULL DEFAULT '',
	is_group INTEGER NOT NULL DEFAULT 0,
	last_ts  INTEGER NOT NULL DEFAULT 0,
	unread   INTEGER NOT NULL DEFAULT 0,
	preview  TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS wa3ds_messages (
	chat        TEXT NOT NULL,
	id          TEXT NOT NULL,
	ts          INTEGER NOT NULL,
	from_me     INTEGER NOT NULL,
	sender      TEXT NOT NULL DEFAULT '',
	sender_name TEXT NOT NULL DEFAULT '',
	kind        TEXT NOT NULL,
	text        TEXT NOT NULL DEFAULT '',
	media       BLOB,
	PRIMARY KEY (chat, id)
);
CREATE INDEX IF NOT EXISTS wa3ds_messages_chat_ts ON wa3ds_messages (chat, ts);
`

func NewStore(ctx context.Context, db *sql.DB) (*Store, error) {
	if _, err := db.ExecContext(ctx, schema); err != nil {
		return nil, err
	}
	return &Store{db: db}, nil
}

// EnsureChat creates the chat row if missing and updates the name when a non-empty one is given.
func (s *Store) EnsureChat(ctx context.Context, jid, name string, isGroup bool) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO wa3ds_chats (jid, name, is_group) VALUES (?, ?, ?)
		ON CONFLICT (jid) DO UPDATE SET name = CASE WHEN excluded.name <> '' THEN excluded.name ELSE wa3ds_chats.name END`,
		jid, name, isGroup)
	return err
}

func (s *Store) SetUnread(ctx context.Context, jid string, unread int) error {
	_, err := s.db.ExecContext(ctx, `UPDATE wa3ds_chats SET unread = ? WHERE jid = ?`, unread, jid)
	return err
}

func (s *Store) GetChat(ctx context.Context, jid string) (Chat, error) {
	var c Chat
	err := s.db.QueryRowContext(ctx,
		`SELECT jid, name, is_group, last_ts, unread, preview FROM wa3ds_chats WHERE jid = ?`, jid).
		Scan(&c.JID, &c.Name, &c.IsGroup, &c.LastTS, &c.Unread, &c.Preview)
	return c, err
}

func (s *Store) ChatName(ctx context.Context, jid string) string {
	var name string
	_ = s.db.QueryRowContext(ctx, `SELECT name FROM wa3ds_chats WHERE jid = ?`, jid).Scan(&name)
	return name
}

// Chats returns chats that have at least one message, newest first.
func (s *Store) Chats(ctx context.Context, limit int) ([]Chat, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT jid, name, is_group, last_ts, unread, preview FROM wa3ds_chats
		WHERE last_ts > 0 ORDER BY last_ts DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Chat
	for rows.Next() {
		var c Chat
		if err := rows.Scan(&c.JID, &c.Name, &c.IsGroup, &c.LastTS, &c.Unread, &c.Preview); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// PutMessage inserts or replaces a message and moves the chat's preview forward
// when the message is the newest one. bumpUnread increments the unread counter.
func (s *Store) PutMessage(ctx context.Context, m Msg, preview string, bumpUnread bool) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()

	res, err := tx.ExecContext(ctx, `
		INSERT INTO wa3ds_messages (chat, id, ts, from_me, sender, sender_name, kind, text, media)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT (chat, id) DO NOTHING`,
		m.Chat, m.ID, m.TS, m.FromMe, m.Sender, m.SenderName, m.Kind, m.Text, m.Media)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		// Already stored (e.g. history sync overlap); nothing else to update.
		return tx.Commit()
	}
	if _, err := tx.ExecContext(ctx, `
		UPDATE wa3ds_chats SET last_ts = ?, preview = ? WHERE jid = ? AND last_ts <= ?`,
		m.TS, preview, m.Chat, m.TS); err != nil {
		return err
	}
	if bumpUnread {
		if _, err := tx.ExecContext(ctx, `UPDATE wa3ds_chats SET unread = unread + 1 WHERE jid = ?`, m.Chat); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// UpdateMessageText changes the text/kind of an existing message (edit or delete).
// It returns the updated message, or sql.ErrNoRows if the message is unknown.
func (s *Store) UpdateMessageText(ctx context.Context, chat, id, kind, text string, dropMedia bool) (Msg, error) {
	q := `UPDATE wa3ds_messages SET kind = ?, text = ? WHERE chat = ? AND id = ?`
	if dropMedia {
		q = `UPDATE wa3ds_messages SET kind = ?, text = ?, media = NULL WHERE chat = ? AND id = ?`
	}
	res, err := s.db.ExecContext(ctx, q, kind, text, chat, id)
	if err != nil {
		return Msg{}, err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return Msg{}, sql.ErrNoRows
	}
	return s.GetMessage(ctx, chat, id)
}

func (s *Store) GetMessage(ctx context.Context, chat, id string) (Msg, error) {
	var m Msg
	err := s.db.QueryRowContext(ctx, `
		SELECT chat, id, ts, from_me, sender, sender_name, kind, text, media
		FROM wa3ds_messages WHERE chat = ? AND id = ?`, chat, id).
		Scan(&m.Chat, &m.ID, &m.TS, &m.FromMe, &m.Sender, &m.SenderName, &m.Kind, &m.Text, &m.Media)
	return m, err
}

// Messages returns up to limit messages older than before (0 = newest), oldest first.
func (s *Store) Messages(ctx context.Context, chat string, before int64, limit int) ([]Msg, error) {
	if before <= 0 {
		before = 1<<62 - 1
	}
	rows, err := s.db.QueryContext(ctx, `
		SELECT chat, id, ts, from_me, sender, sender_name, kind, text, media IS NOT NULL
		FROM wa3ds_messages WHERE chat = ? AND ts < ? ORDER BY ts DESC, rowid DESC LIMIT ?`,
		chat, before, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Msg
	for rows.Next() {
		var m Msg
		var hasMedia bool
		if err := rows.Scan(&m.Chat, &m.ID, &m.TS, &m.FromMe, &m.Sender, &m.SenderName, &m.Kind, &m.Text, &hasMedia); err != nil {
			return nil, err
		}
		if hasMedia {
			m.Media = []byte{1} // marker only; the blob itself is loaded by GetMessage
		}
		out = append(out, m)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out, nil
}

// UnreadIncoming returns the newest incoming message IDs in a chat grouped by sender,
// used to send read receipts.
func (s *Store) UnreadIncoming(ctx context.Context, chat string, limit int) (map[string][]string, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT sender, id FROM wa3ds_messages
		WHERE chat = ? AND from_me = 0 ORDER BY ts DESC LIMIT ?`, chat, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string][]string{}
	for rows.Next() {
		var sender, id string
		if err := rows.Scan(&sender, &id); err != nil {
			return nil, err
		}
		out[sender] = append(out[sender], id)
	}
	return out, rows.Err()
}

var errNotFound = errors.New("not found")

// SetSenderName updates the stored sender name on all messages from one sender.
func (s *Store) SetSenderName(ctx context.Context, sender, name string) error {
	_, err := s.db.ExecContext(ctx, `UPDATE wa3ds_messages SET sender_name = ? WHERE sender = ? AND from_me = 0`, name, sender)
	return err
}

// ChatJIDsOnServer lists chats whose JID is on the given server (e.g. "lid").
func (s *Store) ChatJIDsOnServer(ctx context.Context, server string) ([]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT jid FROM wa3ds_chats WHERE jid LIKE ?`, "%@"+server)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var j string
		if err := rows.Scan(&j); err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, rows.Err()
}

// MergeChat moves all messages of chat from into chat to and combines the chat rows:
// newest preview wins, unread counts add up, and a real name beats a placeholder.
func (s *Store) MergeChat(ctx context.Context, from, to string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()

	get := func(jid string) (Chat, bool) {
		var c Chat
		err := tx.QueryRowContext(ctx, `SELECT jid, name, is_group, last_ts, unread, preview FROM wa3ds_chats WHERE jid = ?`, jid).
			Scan(&c.JID, &c.Name, &c.IsGroup, &c.LastTS, &c.Unread, &c.Preview)
		return c, err == nil
	}
	src, ok := get(from)
	if !ok {
		return nil
	}
	dst, ok := get(to)
	if !ok {
		dst = Chat{JID: to}
		if _, err := tx.ExecContext(ctx, `INSERT INTO wa3ds_chats (jid) VALUES (?)`, to); err != nil {
			return err
		}
	}
	if src.LastTS > dst.LastTS {
		dst.LastTS, dst.Preview = src.LastTS, src.Preview
	}
	dst.Unread += src.Unread
	if isFallbackName(dst.Name) && !isFallbackName(src.Name) {
		dst.Name = src.Name
	}
	if _, err := tx.ExecContext(ctx, `UPDATE OR IGNORE wa3ds_messages SET chat = ? WHERE chat = ?`, to, from); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM wa3ds_messages WHERE chat = ?`, from); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `UPDATE wa3ds_chats SET name = ?, last_ts = ?, unread = ?, preview = ? WHERE jid = ?`,
		dst.Name, dst.LastTS, dst.Unread, dst.Preview, to); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM wa3ds_chats WHERE jid = ?`, from); err != nil {
		return err
	}
	return tx.Commit()
}
