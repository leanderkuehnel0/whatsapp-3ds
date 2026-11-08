package main

import (
	"bufio"
	"context"
	"crypto/subtle"
	"crypto/tls"
	"fmt"
	"io"
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

func (h *Hub) snapshot() []*Conn {
	h.mu.Lock()
	defer h.mu.Unlock()
	conns := make([]*Conn, 0, len(h.clients))
	for c := range h.clients {
		conns = append(conns, c)
	}
	return conns
}

// Broadcast queues an event line for every client. It never blocks on a slow client.
func (h *Hub) Broadcast(l string) {
	for _, c := range h.snapshot() {
		c.enqueue([]byte(l+"\n"), false)
	}
}

// BroadcastAudio queues one call audio frame for every client, dropping it for clients that lag behind.
// It returns how many clients got the frame and how many dropped it.
func (h *Hub) BroadcastAudio(frame []byte) (sent, dropped int) {
	conns := h.snapshot()
	if len(conns) == 0 {
		return 0, 0
	}
	msg := append([]byte(line("*", "AUD", strconv.Itoa(len(frame)))+"\n"), frame...)
	for _, c := range conns {
		if c.enqueue(msg, true) {
			sent++
		} else {
			dropped++
		}
	}
	return sent, dropped
}

// Conn is one connected 3DS. All writes go through a queue drained by one writer goroutine,
// so call audio and events are never held up by a slow connection.
type Conn struct {
	nc   net.Conn
	out  chan []byte
	done chan struct{}
	once sync.Once
}

const (
	writeTimeout = 30 * time.Second
	queueLen     = 512
)

func newConn(nc net.Conn) *Conn {
	c := &Conn{nc: nc, out: make(chan []byte, queueLen), done: make(chan struct{})}
	go c.writer()
	return c
}

func (c *Conn) writer() {
	w := bufio.NewWriterSize(c.nc, 32*1024)
	for {
		select {
		case <-c.done:
			return
		case msg := <-c.out:
			if msg == nil { // shutdown marker: everything before it has been written
				_ = w.Flush()
				c.close()
				return
			}
			_ = c.nc.SetWriteDeadline(time.Now().Add(writeTimeout + time.Duration(len(msg)/4096)*time.Second))
			if _, err := w.Write(msg); err != nil {
				c.close()
				return
			}
			// batch whatever else is already queued into the same flush
			for drained := false; !drained; {
				select {
				case more := <-c.out:
					if more == nil {
						_ = w.Flush()
						c.close()
						return
					}
					if _, err := w.Write(more); err != nil {
						c.close()
						return
					}
				default:
					drained = true
				}
			}
			if err := w.Flush(); err != nil {
				c.close()
				return
			}
		}
	}
}

// enqueue hands a message to the writer. Droppable messages (audio) are discarded when the
// queue is full; others wait briefly and then give up on the connection.
func (c *Conn) enqueue(msg []byte, droppable bool) bool {
	select {
	case <-c.done:
		return false
	default:
	}
	if droppable {
		select {
		case c.out <- msg:
			return true
		default:
			return false
		}
	}
	select {
	case c.out <- msg:
		return true
	case <-c.done:
		return false
	case <-time.After(10 * time.Second):
		c.close()
		return false
	}
}

func (c *Conn) writeLines(lines ...string) error {
	var b []byte
	for _, l := range lines {
		b = append(b, l...)
		b = append(b, '\n')
	}
	if !c.enqueue(b, false) {
		return fmt.Errorf("connection closed")
	}
	return nil
}

func (c *Conn) writeBinary(header string, data []byte) error {
	msg := make([]byte, 0, len(header)+1+len(data))
	msg = append(append(append(msg, header...), '\n'), data...)
	if !c.enqueue(msg, false) {
		return fmt.Errorf("connection closed")
	}
	return nil
}

// shutdown sends whatever is queued, then closes the connection.
func (c *Conn) shutdown() {
	select {
	case c.out <- nil:
		select {
		case <-c.done:
		case <-time.After(5 * time.Second):
		}
	case <-c.done:
	case <-time.After(time.Second):
	}
	c.close()
}

func (c *Conn) close() {
	c.once.Do(func() {
		close(c.done)
		_ = c.nc.Close()
	})
}

// Server accepts TLS connections from 3DS clients and answers protocol requests (see PROTOCOL.md).
type Server struct {
	bridge *Bridge
	token  string
	tlsCfg *tls.Config
}

const (
	idleTimeout   = 3 * time.Minute // client pings every 30 s
	maxLineLen    = 64 * 1024
	maxAudioFrame = 4096
)

// binaryCommands lists requests that carry a payload, with their size limits.
var binaryCommands = map[string]int{
	"AUD":   maxAudioFrame,
	"IMG":   maxImageUpload,
	"VOICE": maxVoiceUpload,
}

// dispatchUpload handles IMG and VOICE requests once their payload has been read.
func (s *Server) dispatchUpload(c *Conn, tag, cmd string, args []string, payload []byte) error {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Minute)
	defer cancel()
	if len(args) < 1 {
		return c.writeLines(line(tag, "ERR", "missing chat"))
	}
	var id string
	var err error
	switch cmd {
	case "IMG":
		caption := ""
		if len(args) > 1 {
			caption = args[1]
		}
		id, err = s.bridge.SendImage(ctx, args[0], payload, caption)
	case "VOICE":
		id, err = s.bridge.SendVoice(ctx, args[0], payload)
	}
	if err != nil {
		log.Printf("%s to %s failed: %v", cmd, args[0], err)
		return c.writeLines(line(tag, "ERR", err.Error()))
	}
	return c.writeLines(line(tag, "OK", id))
}

func firstArg(a []string) string {
	if len(a) == 0 {
		return ""
	}
	return a[0]
}

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
	c := newConn(nc)
	defer c.shutdown()
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

		// Requests followed by a binary payload: "<tag> <CMD> <len> ..." + len bytes.
		if limit, ok := binaryCommands[cmd]; ok && authed {
			n, err := strconv.Atoi(firstArg(args))
			if err != nil || n <= 0 || n > limit {
				log.Printf("client %s: bad %s length %q", remote, cmd, firstArg(args))
				return
			}
			_ = nc.SetReadDeadline(time.Now().Add(idleTimeout + time.Duration(n/2048)*time.Second))
			payload := make([]byte, n)
			if _, err := io.ReadFull(r, payload); err != nil {
				return
			}
			if cmd == "AUD" {
				s.bridge.calls.FeedMicAudio(payload) // call audio: no reply
				continue
			}
			// uploads can take a while; don't block reading the next requests
			go func(tag, cmd string, args []string, payload []byte) {
				if err := s.dispatchUpload(c, tag, cmd, args, payload); err != nil {
					c.close()
				}
			}(tag, cmd, args[1:], payload)
			continue
		}

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
			if l, ok := s.bridge.calls.StateLine(); ok {
				_ = c.writeLines(l)
			}
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
		chats, err := b.store.Chats(ctx, 600)
		if err != nil {
			return errLine(err)
		}
		lines := make([]string, 0, len(chats)+1)
		for _, ch := range chats {
			ch = b.fixChat(ctx, ch)
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
		b.fixSenders(ctx, msgs)
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

	case "VOICEGET":
		if len(args) < 2 {
			return errLine(fmt.Errorf("usage: VOICEGET jid id"))
		}
		m, err := b.store.GetMessage(ctx, args[0], args[1])
		if err != nil {
			return errLine(fmt.Errorf("message not found"))
		}
		frames, err := b.media.Voice(ctx, m)
		if err != nil {
			log.Printf("voice %s/%s: %v", args[0], args[1], err)
			return errLine(err)
		}
		ms := len(frames) / adpcmFrameBytes * voiceFrameMS
		return c.writeBinary(line(tag, "BIN", strconv.Itoa(len(frames)), strconv.Itoa(ms), "0"), frames)

	case "CALL":
		if len(args) < 1 {
			return errLine(fmt.Errorf("usage: CALL jid"))
		}
		id, err := b.calls.Start(ctx, args[0])
		if err != nil {
			return errLine(err)
		}
		return c.writeLines(line(tag, "OK", id))

	case "ACCEPT", "HANGUP":
		if len(args) < 1 {
			return errLine(fmt.Errorf("usage: %s call_id", cmd))
		}
		var err error
		if cmd == "ACCEPT" {
			err = b.calls.Accept(ctx, args[0])
		} else {
			err = b.calls.Hangup(ctx, args[0])
		}
		if err != nil {
			return errLine(err)
		}
		return c.writeLines(line(tag, "OK"))
	}
	return errLine(fmt.Errorf("unknown command %q", cmd))
}
