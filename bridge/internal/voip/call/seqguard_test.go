package call

import "testing"

func TestSeqGuard(t *testing.T) {
	var g seqGuard
	steps := []struct {
		seq  uint16
		want bool
	}{
		{65533, true}, // first packet
		{65534, true},
		{65534, false}, // duplicate
		{0, true},      // 65535 lost; counter wrapped
		{65535, false}, // ... and resent late
		{1, true},
		{3, true}, // gap is fine
		{2, false},
		{4, true},
	}
	for i, s := range steps {
		if got := g.accept(7, s.seq); got != s.want {
			t.Fatalf("step %d: accept(%d) = %v, want %v", i, s.seq, got, s.want)
		}
	}
	if !g.accept(8, 100) {
		t.Fatal("a new SSRC must start fresh")
	}
	// the sender restarts its numbering far behind: accepted again after a short while
	g = seqGuard{}
	g.accept(7, 40000)
	accepted := -1
	for i := 0; i < 2*seqGuardResync; i++ {
		if g.accept(7, uint16(30000+i)) && accepted < 0 {
			accepted = i
		}
	}
	if accepted != seqGuardResync-1 {
		t.Fatalf("resync after %d packets, want %d", accepted+1, seqGuardResync)
	}
	if !g.accept(7, uint16(30000+2*seqGuardResync)) {
		t.Fatal("packets after the resync must be accepted")
	}
}
