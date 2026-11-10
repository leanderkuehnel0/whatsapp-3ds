package call

// seqGuard keeps the incoming audio in order (wa3ds addition).
//
// The relay channel is reliable but unordered: a packet lost between the relay and the bridge
// is resent and turns up a second or more late, after the packets that followed it. Decoding
// it then would play old audio in the wrong place and confuse both the speech decoder, which
// carries state from packet to packet, and the SRTP rollover counter. Packets that are not
// newer than the newest one seen are therefore dropped, as are duplicates.
type seqGuard struct {
	started bool
	ssrc    uint32
	last    uint16
	stale   int
}

// seqGuardResync is how many old-looking packets in a row mean the sender started its
// numbering over (about 3 s of audio), rather than a few stragglers.
const seqGuardResync = 50

// accept reports whether the packet with this RTP sequence number should be decoded.
func (g *seqGuard) accept(ssrc uint32, seq uint16) bool {
	if !g.started || ssrc != g.ssrc {
		*g = seqGuard{started: true, ssrc: ssrc, last: seq}
		return true
	}
	if int16(seq-g.last) > 0 { // newer, allowing for the 16-bit counter wrapping around
		g.last = seq
		g.stale = 0
		return true
	}
	g.stale++
	if g.stale >= seqGuardResync {
		g.last = seq
		g.stale = 0
		return true
	}
	return false
}
