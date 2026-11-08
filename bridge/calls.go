package main

import (
	"context"
	"fmt"
	"log"
	"log/slog"
	"math"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/call"
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/core"
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/signaling"
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/wanode"
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/wa"

	waBinary "go.mau.fi/whatsmeow/binary"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
)

// Call audio between bridge and 3DS: 16 kHz mono, 20 ms ADPCM frames.
const callFrameSamples = 320

// CallHub runs 1:1 voice calls through the vendored WaCalls VoIP stack and
// relays their audio to and from the 3DS. One call at a time is supported.
type CallHub struct {
	b   *Bridge
	log *slog.Logger

	mu    sync.Mutex
	calls map[string]*call.CallManager

	audioMu  sync.Mutex
	encState adpcmState
	peerBuf  []int16 // peer audio waiting to fill a 20 ms frame

	// call audio diagnostics, logged every few seconds during a call
	toClient, clientDrop, noClient, micFrames atomic.Int64
}

func NewCallHub(b *Bridge, debug bool) *CallHub {
	level := slog.LevelWarn
	if debug {
		level = slog.LevelDebug
	}
	return &CallHub{
		b:     b,
		log:   slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})).With("component", "voip"),
		calls: map[string]*call.CallManager{},
	}
}

func wrapCall(from types.JID, inner *waBinary.Node) *waBinary.Node {
	var content []waBinary.Node
	if inner != nil {
		content = append(content, *inner)
	}
	return &waBinary.Node{Tag: "call", Attrs: waBinary.Attrs{"from": from}, Content: content}
}

func callIDOf(from types.JID, data *waBinary.Node) string {
	info := signaling.ExtractNodeInfo(wrapCall(from, data))
	if info == nil {
		return ""
	}
	return info.CallID
}

func (h *CallHub) get(id string) *call.CallManager {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.calls[id]
}

// current returns the call that is not yet over, if any.
func (h *CallHub) current() (string, *call.CallManager) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for id, cm := range h.calls {
		if c := cm.CurrentCall(); c != nil && !c.IsEnded() {
			return id, cm
		}
	}
	return "", nil
}

func (h *CallHub) remove(id string) {
	h.mu.Lock()
	delete(h.calls, id)
	h.mu.Unlock()
}

func (h *CallHub) newManager(id string) *call.CallManager {
	cm := call.NewCallManager(wa.NewSocket(h.b.wa), h.log.With("call_id", id))
	// Callbacks run with the call manager's lock held: only read state and broadcast here.
	cm.OnIncoming = func(c *call.CallInfo) { h.broadcastState(c) }
	cm.OnStateChange = func(c *call.CallInfo) {
		h.broadcastState(c)
		if c.IsEnded() {
			go h.remove(id)
		}
	}
	cm.OnEnded = func(c *call.CallInfo) {
		h.broadcastState(c)
		go h.remove(id)
	}
	cm.OnPeerAudio = h.sendPeerAudio
	h.mu.Lock()
	h.calls[id] = cm
	h.mu.Unlock()
	h.resetAudio()
	go h.logStats(id, cm)
	return cm
}

// logStats logs, every 5 s while a call is active, what happened to its audio in both
// directions. One-way audio shows up as a counter that stays at zero.
func (h *CallHub) logStats(id string, cm *call.CallManager) {
	t := time.NewTicker(5 * time.Second)
	defer t.Stop()
	for range t.C {
		if h.get(id) == nil {
			return
		}
		c := cm.CurrentCall()
		if c == nil || (c.StateData.State != core.CallStateActive && c.StateData.State != core.CallStateConnecting) {
			continue
		}
		st := &cm.Stats
		log.Printf("Call %s audio: whatsapp->bridge pkts=%d stun=%d notrtp=%d otherpt=%d(last %d) ownssrc=%d nosession=%d srtperr=%d empty=%d decodeerr=%d decoded=%d | bridge->3ds frames=%d dropped=%d noclient=%d | 3ds->bridge frames=%d bridge->whatsapp pkts=%d",
			id, st.Received.Load(), st.Stun.Load(), st.NotRtp.Load(), st.OtherPT.Load(), st.LastPT.Load(), st.OwnSsrc.Load(),
			st.NoSession.Load(), st.Unprotect.Load(), st.Empty.Load(), st.Decode.Load(), st.Delivered.Load(),
			h.toClient.Load(), h.clientDrop.Load(), h.noClient.Load(), h.micFrames.Load(), st.Sent.Load())
	}
}

func (h *CallHub) resetAudio() {
	h.audioMu.Lock()
	h.encState = adpcmState{}
	h.peerBuf = h.peerBuf[:0]
	h.audioMu.Unlock()
	h.toClient.Store(0)
	h.clientDrop.Store(0)
	h.noClient.Store(0)
	h.micFrames.Store(0)
}

// HandleEvent routes WhatsApp call signaling. It reports whether the event was a call event.
func (h *CallHub) HandleEvent(ctx context.Context, rawEvt any) bool {
	switch evt := rawEvt.(type) {
	case *events.CallOffer:
		id := callIDOf(evt.From, evt.Data)
		if id == "" {
			return true
		}
		if cur, _ := h.current(); cur != "" {
			h.rejectBusy(ctx, evt.From, evt.Data)
			return true
		}
		log.Printf("Incoming call %s from %s", id, evt.From)
		h.newManager(id).HandleCallOffer(ctx, wrapCall(evt.From, evt.Data), evt.From)
	case *events.CallAccept:
		if cm := h.get(callIDOf(evt.From, evt.Data)); cm != nil {
			cm.HandleCallAccept(ctx, wrapCall(evt.From, evt.Data), evt.From)
		}
	case *events.CallTransport:
		if cm := h.get(callIDOf(evt.From, evt.Data)); cm != nil {
			cm.HandleCallTransport(ctx, wrapCall(evt.From, evt.Data), evt.From)
		}
	case *events.CallTerminate:
		if cm := h.get(callIDOf(evt.From, evt.Data)); cm != nil {
			cm.HandleCallTerminate(wrapCall(evt.From, evt.Data))
		}
	case *events.CallReject:
		if cm := h.get(callIDOf(evt.From, evt.Data)); cm != nil {
			cm.HandleCallTerminate(wrapCall(evt.From, evt.Data))
		}
	default:
		return false
	}
	return true
}

func (h *CallHub) rejectBusy(ctx context.Context, from types.JID, data *waBinary.Node) {
	info := signaling.ExtractNodeInfo(wrapCall(from, data))
	if info == nil {
		return
	}
	creator := wanode.AttrString(info.InnerNode.Attrs, "call-creator")
	if creator == "" {
		creator = from.String()
	}
	_ = wa.NewSocket(h.b.wa).SendNode(ctx, signaling.BuildRejectStanza(from, info.CallID, wanode.MustJID(creator)))
	log.Printf("Rejected call %s from %s: already in a call", info.CallID, from)
}

// Start places an outgoing voice call and returns its ID.
func (h *CallHub) Start(ctx context.Context, chatJID string) (string, error) {
	jid, err := types.ParseJID(chatJID)
	if err != nil {
		return "", fmt.Errorf("bad jid: %w", err)
	}
	if jid.Server == types.GroupServer {
		return "", fmt.Errorf("group calls are not supported")
	}
	if cur, _ := h.current(); cur != "" {
		return "", fmt.Errorf("already in a call")
	}
	id := signaling.GenerateCallID()
	cm := h.newManager(id)
	if err := cm.StartCall(ctx, id, jid, false); err != nil {
		h.remove(id)
		return "", err
	}
	log.Printf("Calling %s (call %s)", jid, id)
	return id, nil
}

func (h *CallHub) Accept(ctx context.Context, id string) error {
	cm := h.get(id)
	if cm == nil {
		return fmt.Errorf("no such call")
	}
	return cm.AcceptCall(ctx, id)
}

// Hangup declines a ringing incoming call or ends any other call.
func (h *CallHub) Hangup(ctx context.Context, id string) error {
	cm := h.get(id)
	if cm == nil {
		return fmt.Errorf("no such call")
	}
	c := cm.CurrentCall()
	if c != nil && c.Direction == core.CallDirectionIncoming && c.StateData.State == core.CallStateIncomingRinging {
		return cm.RejectCall(ctx, id, core.EndCallReasonDeclined)
	}
	return cm.EndCall(ctx, core.EndCallReasonUserEnded)
}

// stateFields maps a call to the CALLSTATE record (see PROTOCOL.md).
func (h *CallHub) stateFields(c *call.CallInfo) []string {
	ctx := context.Background()
	chat := c.PeerJid
	name := c.PeerJid
	if jid, err := types.ParseJID(c.PeerJid); err == nil {
		norm := h.b.normalizeChat(ctx, jid)
		chat = norm.String()
		name = h.b.contactName(ctx, norm)
	}
	state := "ended"
	switch c.StateData.State {
	case core.CallStateInitiating, core.CallStateRinging:
		state = "ringing"
	case core.CallStateIncomingRinging:
		state = "incoming"
	case core.CallStateConnecting:
		state = "connecting"
	case core.CallStateActive, core.CallStateOnHold:
		state = "active"
	}
	dir := "out"
	if c.Direction == core.CallDirectionIncoming {
		dir = "in"
	}
	return []string{c.CallID, chat, name, state, dir, string(c.StateData.EndReason)}
}

func (h *CallHub) broadcastState(c *call.CallInfo) {
	f := h.stateFields(c)
	log.Printf("Call %s: %s", c.CallID, f[3])
	h.b.hub.Broadcast(line(append([]string{"*", "CALLSTATE"}, f...)...))
}

// StateLine returns the CALLSTATE event for an ongoing call, for newly connected clients.
func (h *CallHub) StateLine() (string, bool) {
	_, cm := h.current()
	if cm == nil {
		return "", false
	}
	c := cm.CurrentCall()
	if c == nil {
		return "", false
	}
	return line(append([]string{"*", "CALLSTATE"}, h.stateFields(c)...)...), true
}

// sendPeerAudio encodes decoded WhatsApp audio (16 kHz float PCM) and pushes it to the 3DS.
func (h *CallHub) sendPeerAudio(pcm []float32) {
	h.audioMu.Lock()
	for _, s := range pcm {
		v := math.Round(float64(s) * 32767)
		h.peerBuf = append(h.peerBuf, int16(max(-32768, min(32767, v))))
	}
	var frames [][]byte
	for len(h.peerBuf) >= callFrameSamples {
		frames = append(frames, adpcmEncode(&h.encState, h.peerBuf[:callFrameSamples]))
		h.peerBuf = h.peerBuf[callFrameSamples:]
	}
	h.peerBuf = append(h.peerBuf[:0:0], h.peerBuf...)
	h.audioMu.Unlock()
	for _, f := range frames {
		sent, dropped := h.b.hub.BroadcastAudio(f)
		h.toClient.Add(int64(sent))
		h.clientDrop.Add(int64(dropped))
		if sent+dropped == 0 {
			h.noClient.Add(1)
		}
	}
}

// FeedMicAudio takes one ADPCM frame from the 3DS microphone and sends it into the call.
func (h *CallHub) FeedMicAudio(frame []byte) {
	_, cm := h.current()
	if cm == nil {
		return
	}
	samples := adpcmDecode(frame)
	if samples == nil {
		return
	}
	h.micFrames.Add(1)
	pcm := make([]float32, len(samples))
	for i, s := range samples {
		pcm[i] = float32(s) / 32768
	}
	cm.FeedCapturedPCM(pcm)
}
