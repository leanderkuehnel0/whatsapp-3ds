package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"database/sql"
	"net"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store/sqlstore"
)

// newTestServer starts the TLS server backed by a fresh database and an unlinked WhatsApp device.
func newTestServer(t *testing.T) (addr string, b *Bridge, token string) {
	t.Helper()
	ctx := context.Background()
	dir := t.TempDir()
	db, err := sql.Open("sqlite", "file:"+filepath.Join(dir, "t.db")+"?_pragma=foreign_keys(1)")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	container := sqlstore.NewWithDB(db, "sqlite3", nil)
	if err := container.Upgrade(ctx); err != nil {
		t.Fatal(err)
	}
	st, err := NewStore(ctx, db)
	if err != nil {
		t.Fatal(err)
	}
	dev, err := container.GetFirstDevice(ctx)
	if err != nil {
		t.Fatal(err)
	}
	client := whatsmeow.NewClient(dev, nil)
	b = &Bridge{wa: client, store: st, hub: NewHub(), media: &MediaCache{dir: filepath.Join(dir, "media"), wa: client}}
	b.calls = NewCallHub(b, false)

	cert, err := loadOrCreateCert(dir)
	if err != nil {
		t.Fatal(err)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	token = "testtoken"
	srv := &Server{bridge: b, token: token, tlsCfg: &tls.Config{Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS12}}
	go srv.Serve(ln)
	return ln.Addr().String(), b, token
}

type testClient struct {
	t *testing.T
	c *tls.Conn
	r *bufio.Reader
}

func dial(t *testing.T, addr string) *testClient {
	t.Helper()
	// Like the 3DS: no CA validation (the real client pins the fingerprint) and TLS 1.2 only.
	c, err := tls.Dial("tcp", addr, &tls.Config{InsecureSkipVerify: true, MaxVersion: tls.VersionTLS12})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return &testClient{t: t, c: c, r: bufio.NewReader(c)}
}

func (tc *testClient) send(fields ...string) {
	tc.t.Helper()
	if _, err := tc.c.Write([]byte(line(fields...) + "\n")); err != nil {
		tc.t.Fatal(err)
	}
}

func (tc *testClient) recv() []string {
	tc.t.Helper()
	_ = tc.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	l, err := tc.r.ReadString('\n')
	if err != nil {
		tc.t.Fatalf("read: %v", err)
	}
	return splitLine(l)
}

// recvUntil reads lines until one with the given tag and kind, skipping events.
func (tc *testClient) recvUntil(tag, kind string) (got [][]string) {
	tc.t.Helper()
	for {
		f := tc.recv()
		if f[0] == "*" {
			continue
		}
		got = append(got, f)
		if f[0] == tag && f[1] == kind {
			return got
		}
		if f[0] == tag && f[1] == "ERR" && kind != "ERR" {
			tc.t.Fatalf("unexpected error: %q", f)
		}
	}
}

func TestRejectsBadToken(t *testing.T) {
	addr, _, _ := newTestServer(t)
	c := dial(t, addr)
	c.send("1", "AUTH", "wrong")
	if f := c.recv(); f[1] != "ERR" {
		t.Fatalf("expected ERR, got %q", f)
	}
	// connection must be closed afterwards
	_ = c.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, err := c.r.ReadString('\n'); err == nil {
		t.Fatal("connection still open after bad token")
	}
}

func TestRequiresAuthFirst(t *testing.T) {
	addr, _, _ := newTestServer(t)
	c := dial(t, addr)
	c.send("1", "CHATS")
	if f := c.recv(); f[1] != "ERR" {
		t.Fatalf("expected ERR, got %q", f)
	}
}

func TestChatsMessagesAndEvents(t *testing.T) {
	addr, b, token := newTestServer(t)
	ctx := context.Background()

	chat := "491701234567@s.whatsapp.net"
	if err := b.store.EnsureChat(ctx, chat, "Alice", false); err != nil {
		t.Fatal(err)
	}
	for i, txt := range []string{"first", "multi\nline\twith tab", "third"} {
		m := Msg{Chat: chat, ID: "id" + string(rune('A'+i)), TS: int64(1000 + i), Sender: chat, SenderName: "Alice", Kind: "text", Text: txt}
		if err := b.store.PutMessage(ctx, m, previewOf(m, false), true); err != nil {
			t.Fatal(err)
		}
	}

	c := dial(t, addr)
	c.send("1", "AUTH", token)
	auth := c.recv()
	if auth[0] != "1" || auth[1] != "OK" || len(auth) != 5 {
		t.Fatalf("auth reply: %q", auth)
	}

	c.send("2", "CHATS")
	rows := c.recvUntil("2", "END")
	if len(rows) != 2 || rows[0][1] != "CHAT" {
		t.Fatalf("chats: %q", rows)
	}
	ch := rows[0][2:]
	if ch[0] != chat || ch[1] != "Alice" || ch[3] != "1002" || ch[4] != "3" || ch[5] != "third" {
		t.Fatalf("chat row: %q", ch)
	}

	c.send("3", "MSGS", chat, "0", "2")
	rows = c.recvUntil("3", "END")
	if len(rows) != 3 {
		t.Fatalf("msgs: %q", rows)
	}
	if rows[0][3] != "idB" || rows[0][9] != "multi\nline\twith tab" || rows[1][3] != "idC" {
		t.Fatalf("expected the two newest messages oldest-first with text intact, got %q", rows)
	}

	c.send("4", "MSGS", chat, "1001", "10")
	rows = c.recvUntil("4", "END")
	if len(rows) != 2 || rows[0][3] != "idA" {
		t.Fatalf("older page: %q", rows)
	}

	// a live message is pushed as an event
	m := Msg{Chat: chat, ID: "idD", TS: 2000, Sender: chat, SenderName: "Alice", Kind: "text", Text: "live"}
	if err := b.store.PutMessage(ctx, m, previewOf(m, false), true); err != nil {
		t.Fatal(err)
	}
	b.hub.Broadcast(line(append([]string{"*", "MSG"}, m.fields()...)...))
	for {
		f := c.recv()
		if f[0] == "*" && f[1] == "MSG" {
			if f[3] != "idD" || f[9] != "live" {
				t.Fatalf("event: %q", f)
			}
			break
		}
	}

	c.send("5", "PING")
	if f := c.recvUntil("5", "OK"); len(f) != 1 {
		t.Fatalf("ping: %q", f)
	}

	c.send("6", "MEDIA", chat, "idA")
	f := c.recv()
	for f[0] == "*" {
		f = c.recv()
	}
	if f[0] != "6" || f[1] != "ERR" {
		t.Fatalf("media for text message should fail cleanly, got %q", f)
	}

	c.send("7", "BOGUS")
	f = c.recv()
	if f[1] != "ERR" || !strings.Contains(f[2], "unknown") {
		t.Fatalf("unknown command: %q", f)
	}
}

func TestDescribe(t *testing.T) {
	if kind, _, _, ok := describe(nil); ok || kind != "" {
		t.Fatal("nil message should be skipped")
	}
}

func TestCallCommandsAndAudioFraming(t *testing.T) {
	addr, _, token := newTestServer(t)
	c := dial(t, addr)
	c.send("1", "AUTH", token)
	c.recvUntil("1", "OK")

	c.send("2", "CALL", "123456789@g.us")
	if f := c.recvUntil("2", "ERR"); !strings.Contains(f[len(f)-1][2], "group") {
		t.Fatalf("group call: %q", f)
	}
	c.send("3", "HANGUP", "NOSUCHCALL")
	if f := c.recvUntil("3", "ERR"); len(f) != 1 {
		t.Fatalf("hangup: %q", f)
	}

	// a microphone frame with no call running is ignored, and the stream stays in sync
	var st adpcmState
	frame := adpcmEncode(&st, make([]int16, callFrameSamples))
	if _, err := c.c.Write(append([]byte(line("a", "AUD", "164")+"\n"), frame...)); err != nil {
		t.Fatal(err)
	}
	c.send("4", "PING")
	c.recvUntil("4", "OK")

	// an absurd audio length closes the connection
	c.send("a", "AUD", "999999")
	_ = c.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		if _, err := c.r.ReadString('\n'); err != nil {
			break
		}
	}
}

func TestIsFallbackName(t *testing.T) {
	for _, s := range []string{"", "+491701234567", "123456789012345", "~1234", "+1∙∙∙∙80", "Group"} {
		if !isFallbackName(s) {
			t.Errorf("%q should be a placeholder", s)
		}
	}
	for _, s := range []string{"Alice", "Mom ❤️", "4 Non Blondes"} {
		if isFallbackName(s) {
			t.Errorf("%q is a real name", s)
		}
	}
}

func TestMergeChat(t *testing.T) {
	_, b, _ := newTestServer(t)
	ctx := context.Background()
	lid, pn := "1234567890@lid", "491701234567@s.whatsapp.net"
	_ = b.store.EnsureChat(ctx, lid, "Alice", false)
	_ = b.store.EnsureChat(ctx, pn, "+491701234567", false)
	put := func(chat, id string, ts int64) {
		m := Msg{Chat: chat, ID: id, TS: ts, Kind: "text", Text: id}
		if err := b.store.PutMessage(ctx, m, id, true); err != nil {
			t.Fatal(err)
		}
	}
	put(pn, "old", 100)
	put(lid, "new", 200)
	put(lid, "old", 100) // duplicate of a message already in the PN chat

	if err := b.store.MergeChat(ctx, lid, pn); err != nil {
		t.Fatal(err)
	}
	c, err := b.store.GetChat(ctx, pn)
	if err != nil {
		t.Fatal(err)
	}
	if c.Name != "Alice" || c.LastTS != 200 || c.Preview != "new" || c.Unread != 3 {
		t.Fatalf("merged chat: %+v", c)
	}
	if _, err := b.store.GetChat(ctx, lid); err == nil {
		t.Fatal("LID chat should be gone")
	}
	msgs, _ := b.store.Messages(ctx, pn, 0, 10)
	if len(msgs) != 2 {
		t.Fatalf("expected 2 messages after merge, got %d", len(msgs))
	}
}

func TestMediaUploadFraming(t *testing.T) {
	addr, b, token := newTestServer(t)
	ctx := context.Background()
	chat := "491701234567@s.whatsapp.net"
	_ = b.store.EnsureChat(ctx, chat, "Alice", false)
	_ = b.store.PutMessage(ctx, Msg{Chat: chat, ID: "t1", TS: 1, Kind: "text", Text: "hi"}, "hi", false)

	c := dial(t, addr)
	c.send("1", "AUTH", token)
	c.recvUntil("1", "OK")

	send := func(tag, cmd string, payload []byte, args ...string) {
		t.Helper()
		head := line(append([]string{tag, cmd, strconv.Itoa(len(payload))}, args...)...) + "\n"
		if _, err := c.c.Write(append([]byte(head), payload...)); err != nil {
			t.Fatal(err)
		}
	}

	// not a JPEG: rejected before anything is uploaded
	send("2", "IMG", []byte("definitely not a jpeg"), chat, "caption\twith tab")
	if f := c.recvUntil("2", "ERR"); !strings.Contains(f[len(f)-1][2], "JPEG") {
		t.Fatalf("img: %q", f)
	}
	// voice data that isn't whole ADPCM frames
	send("3", "VOICE", make([]byte, 100), chat)
	if f := c.recvUntil("3", "ERR"); !strings.Contains(f[len(f)-1][2], "multiple") {
		t.Fatalf("voice: %q", f)
	}
	// asking for the audio of a text message
	c.send("4", "VOICEGET", chat, "t1")
	c.recvUntil("4", "ERR")
	// the stream is still in sync after the binary payloads
	c.send("5", "PING")
	c.recvUntil("5", "OK")

	// an oversized upload closes the connection instead of allocating it
	c.send("6", "IMG", strconv.Itoa(maxImageUpload+1), chat)
	_ = c.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		if _, err := c.r.ReadString('\n'); err != nil {
			break
		}
	}
}
