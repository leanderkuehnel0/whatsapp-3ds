package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"database/sql"
	"net"
	"path/filepath"
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
		if f[0] == tag && f[1] == "ERR" {
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
