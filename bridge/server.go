package main

import (
	"bufio"
	"context"
	"crypto/subtle"
	"crypto/tls"
	"fmt"
	"log"
	"net"
	"strconv"
	"sync"
	"time"
)

// Hub tracks authenticated client connections so events can be pushed to all of them.
type Hub struct {
	mu      sync.Mutex
	clients map[*Conn]struct{}
}

func NewHub() *Hub { return &Hub{clients: map[*Conn]struct{}{}} }

func (h *Hub) add(c *Conn)    { h.mu.Lock(); h.clients[c] = struct{}{}; h.mu.Unlock() }
func (h *Hub) remove(c *Conn) { h.mu.Lock(); delete(h.clients, c); h.mu.Unlock() }

func (h *Hub) Broadcast(l string) {
	h.mu.Lock()
	conns := make([]*Conn, 0, len(h.clients))
	for c := range h.clients {
		conns = append(conns, c)
	}
	h.mu.Unlock()
	for _, c := range conns {
		if err := c.writeLines(l); err != nil {
			c.close()
		}
	}
}

// Conn is one connected 3DS.
type Conn struct {
	nc   net.Conn
	wmu  sync.Mutex
	w    *bufio.Writer
	once sync.Once
}

const writeTimeout = 30 * time.Second

func (c *Conn) writeLines(lines ...string) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	_ = c.nc.SetWriteDeadline(time.Now().Add(writeTimeout))
	for _, l := range lines {
		if _, err := c.w.WriteString(l + "\n"); err != nil {
			return err
		}
	}
	return c.w.Flush()
}

func (c *Conn) writeBinary(header string, data []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	_ = c.nc.SetWriteDeadline(time.Now().Add(writeTimeout + time.Duration(len(data)/4096)*time.Second))
	if _, err := c.w.WriteString(header + "\n"); err != nil {
		return err
	}
	if _, err := c.w.Write(data); err != nil {
		return err
	}
	return c.w.Flush()
}

func (c *Conn) close() { c.once.Do(func() { _ = c.nc.Close() }) }

// Server accepts TLS connections from 3DS clients and answers protocol requests (see PROTOCOL.md).
type Server struct {
	bridge *Bridge
	token  string
	tlsCfg *tls.Config
}

const (
	idleTimeout = 3 * time.Minute // client pings every 30 s
	maxLineLen  = 64 * 1024
)

func (s *Server) Serve(ln net.Listener) error {
	tl := tls.NewListener(ln, s.tlsCfg)
	for {
		nc, err := tl.Accept()
		if err != nil {
			return err
		}
		go s.handle(nc)
	}
}

func (s *Server) handle(nc net.Conn) {
	c := &Conn{nc: nc, w: bufio.NewWriter(nc)}
	defer c.close()
	remote := nc.RemoteAddr().String()

	r := bufio.NewReaderSize(nc, 4096)
	authed := false
	defer func() {
		if authed {
			s.bridge.hub.remove(c)
			log.Printf("3DS disconnected: %s", remote)
		}
	}()

	for {
		_ = nc.SetReadDeadline(time.Now().Add(idleTimeout))
		raw, err := readLine(r)
		if err != nil {
			return
		}
		f := splitLine(raw)
		if len(f) < 2 {
			continue
		}
		tag, cmd, args := f[0], f[1], f[2:]

		if !authed {
			if cmd != "AUTH" || len(args) < 1 || subtle.ConstantTimeCompare([]byte(args[0]), []byte(s.token)) != 1 {
				log.Printf("rejected client %s: bad or missing token", remote)
				time.Sleep(2 * time.Second) // slow down guessing
				_ = c.writeLines(line(tag, "ERR", "unauthorized"))
				return
			}
			authed = true
			name, jid := s.bridge.OwnInfo()
			if err := c.writeLines(line(tag, "OK", name, jid, strconv.FormatInt(time.Now().Unix(), 10))); err != nil {
				return
			}
			s.bridge.hub.add(c)
			log.Printf("3DS connected: %s", remote)
			state := "disconnected"
			if s.bridge.wa.IsConnected() {
				state = "connected"
			}
			_ = c.writeLines(line("*", "STATUS", state))
			continue
		}

		if err := s.dispatch(c, tag, cmd, args); err != nil {
			return
		}
	}
}

// readLine reads one line, refusing absurdly long ones so a client can't exhaust memory.
func readLine(r *bufio.Reader) (string, error) {
	var buf []byte
	for {
		chunk, isPrefix, err := r.ReadLine()
		if err != nil {
			return "", err
		}
		buf = append(buf, chunk...)
		if len(buf) > maxLineLen {
			return "", fmt.Errorf("line too long")
		}
		if !isPrefix {
			return string(buf), nil
		}
	}
}

// dispatch runs one request. A returned error means the connection is broken.
func (s *Server) dispatch(c *Conn, tag, cmd string, args []string) error {
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	b := s.bridge
	errLine := func(err error) error { return c.writeLines(line(tag, "ERR", err.Error())) }

	switch cmd {
	case "PING":
		return c.writeLines(line(tag, "OK"))

	case "CHATS":
		chats, err := b.store.Chats(ctx, 200)
		if err != nil {
			return errLine(err)
		}
		lines := make([]string, 0, len(chats)+1)
		for _, ch := range chats {
			lines = append(lines, line(append([]string{tag, "CHAT"}, ch.fields()...)...))
		}
		return c.writeLines(append(lines, line(tag, "END"))...)

	case "MSGS":
		if len(args) < 3 {
			return errLine(fmt.Errorf("usage: MSGS jid before limit"))
		}
		before, _ := strconv.ParseInt(args[1], 10, 64)
		limit, _ := strconv.Atoi(args[2])
		limit = min(max(limit, 1), 100)
		msgs, err := b.store.Messages(ctx, args[0], before, limit)
		if err != nil {
			return errLine(err)
		}
		lines := make([]string, 0, len(msgs)+1)
		for _, m := range msgs {
			lines = append(lines, line(append([]string{tag, "MSG"}, m.fields()...)...))
		}
		return c.writeLines(append(lines, line(tag, "END"))...)

	case "SEND":
		if len(args) < 2 || args[1] == "" {
			return errLine(fmt.Errorf("usage: SEND jid text"))
		}
		id, err := b.SendText(ctx, args[0], args[1])
		if err != nil {
			return errLine(err)
		}
		return c.writeLines(line(tag, "OK", id))

	case "READ":
		if len(args) < 1 {
			return errLine(fmt.Errorf("usage: READ jid"))
		}
		if err := b.MarkChatRead(ctx, args[0]); err != nil {
			return errLine(err)
		}
		return c.writeLines(line(tag, "OK"))

	case "MEDIA":
		if len(args) < 2 {
			return errLine(fmt.Errorf("usage: MEDIA jid id"))
		}
		m, err := b.store.GetMessage(ctx, args[0], args[1])
		if err != nil {
			return errLine(fmt.Errorf("message not found"))
		}
		img, err := b.media.Get(ctx, m)
		if err != nil {
			log.Printf("media %s/%s: %v", args[0], args[1], err)
			return errLine(err)
		}
		return c.writeBinary(line(tag, "BIN", strconv.Itoa(len(img.JPEG)), strconv.Itoa(img.W), strconv.Itoa(img.H)), img.JPEG)
	}
	return errLine(fmt.Errorf("unknown command %q", cmd))
}
