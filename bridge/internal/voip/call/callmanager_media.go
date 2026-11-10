package call

import (
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/core"
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/media"
	"github.com/leanderkuehnel0/whatsapp-3ds/bridge/internal/voip/transport"
	"sync/atomic"
	"time"
)

func (m *CallManager) initCodec() {
	if m.codec != nil {
		return
	}
	codec, err := media.NewMLowCodec(media.DefaultCodecOptions)
	if err != nil {
		m.log.Warn("MLow codec unavailable — call will run signaling-only (no audio)", "err", err)
		return
	}
	m.codec = codec
}

func (m *CallManager) FeedCapturedPCM(data []float32) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if m.codec == nil || m.rtpSession == nil || m.srtpSession == nil || !m.relay.HasConnection() {
		return
	}
	m.lastCaptureAt = time.Now()
	frameSize := m.codec.FrameSize()
	if m.encodeBuf == nil {
		m.encodeBuf = make([]float32, frameSize)
		m.encodeBufPos = 0
	}

	offset := 0
	for offset < len(data) {
		toCopy := min(len(data)-offset, frameSize-m.encodeBufPos)
		copy(m.encodeBuf[m.encodeBufPos:], data[offset:offset+toCopy])
		m.encodeBufPos += toCopy
		offset += toCopy
		if m.encodeBufPos < frameSize {
			break
		}
		frame := make([]float32, frameSize)
		copy(frame, m.encodeBuf)
		m.encodeBufPos = 0

		opus, err := m.codec.Encode(frame)
		if err != nil {
			m.log.Debug("encode error", "err", err)
			continue
		}
		m.sendOpusFrameLocked(opus)
	}
}

func (m *CallManager) sendOpusFrameLocked(opus []byte) {
	if m.rtpSession == nil || m.srtpSession == nil {
		return
	}
	marker := !m.firstPacketSent
	pkt := m.rtpSession.CreatePacketWithDuration(opus, m.codec.FrameSize(), marker)
	if m.debeEnabled {
		pkt.Header.Extension = true
		pkt.Header.ExtensionProfile = 0xbede
		pkt.Header.ExtensionData = nil
	}
	m.firstPacketSent = true

	srtp, err := m.srtpSession.Protect(pkt)
	if err != nil {
		m.log.Debug("srtp protect error", "err", err)
		return
	}
	m.relay.Broadcast(srtp)
	m.Stats.Sent.Add(1)
}

func (m *CallManager) startSilenceKeepaliveLocked() {
	if m.keepaliveStop != nil || m.codec == nil {
		return
	}
	stop := make(chan struct{})
	m.keepaliveStop = stop
	frameSize := m.codec.FrameSize()
	go func() {
		ticker := time.NewTicker(60 * time.Millisecond)
		defer ticker.Stop()
		silence := make([]float32, frameSize)
		for {
			select {
			case <-stop:
				return
			case <-ticker.C:
				m.mu.Lock()
				ready := m.codec != nil && m.rtpSession != nil && m.srtpSession != nil && m.relay.HasConnection()
				// wa3ds: the microphone arrives over TCP from the console's WiFi and stalls
				// for a few hundred ms now and then before catching up. Filling such a
				// stall with silence put a hole into the middle of a sentence; only fill in
				// once the microphone is really off (muted).
				idle := time.Since(m.lastCaptureAt) > 400*time.Millisecond
				if ready && idle {
					if opus, err := m.codec.Encode(silence); err == nil {
						m.sendOpusFrameLocked(opus)
					}
				}
				m.mu.Unlock()
			}
		}
	}()
}

// MediaStats counts what happens to packets received from the relay (wa3ds addition).
type MediaStats struct {
	Received, Stun, NotRtp, OtherPT, OwnSsrc, Late, NoSession, Unprotect, Empty, Decode, Delivered, Sent atomic.Int64
	LastPT                                                                                               atomic.Int32
}

func (m *CallManager) onRelayData(data []byte) {
	m.Stats.Received.Add(1)
	if transport.IsStunPacket(data) {
		m.Stats.Stun.Add(1)
		return
	}
	if !transport.IsRtpPacket(data) || len(data) < 12 {
		m.Stats.NotRtp.Add(1)
		return
	}
	pt := data[1] & 0x7f
	if pt != core.PayloadTypeWhatsAppOpus {
		m.Stats.OtherPT.Add(1)
		m.Stats.LastPT.Store(int32(pt))
		return
	}

	m.mu.Lock()
	if m.srtpSession == nil || m.codec == nil {
		m.mu.Unlock()
		m.Stats.NoSession.Add(1)
		return
	}
	ssrc := uint32(data[8])<<24 | uint32(data[9])<<16 | uint32(data[10])<<8 | uint32(data[11])
	if ssrc == m.selfSsrc {
		m.mu.Unlock()
		m.Stats.OwnSsrc.Add(1)
		return
	}
	if !m.rxOrder.accept(ssrc, uint16(data[2])<<8|uint16(data[3])) {
		m.mu.Unlock()
		m.Stats.Late.Add(1)
		return
	}
	if !m.actualPeerSet {
		m.actualPeerSet = true
		if !containsSsrc(m.peerSsrcs, ssrc) {
			m.peerSsrcs = []uint32{ssrc}
			m.relay.SetSubscriptionSsrc(ssrc)
			go m.relay.ResendSubscriptions()
		}
	}
	srtp := m.srtpSession
	codec := m.codec
	m.mu.Unlock()

	pkt, err := srtp.Unprotect(data)
	if err != nil {
		m.log.Debug("srtp unprotect error", "err", err)
		m.Stats.Unprotect.Add(1)
		return
	}
	if len(pkt.Payload) == 0 {
		m.Stats.Empty.Add(1)
		return
	}
	pcm, err := codec.Decode(pkt.Payload)
	if err != nil {
		m.Stats.Decode.Add(1)
		return
	}
	m.Stats.Delivered.Add(1)
	pcm = media.NormalizeFrame(pcm, codec.FrameSize())
	if m.OnPeerAudio != nil {
		m.OnPeerAudio(pcm)
	}
}
