package main

import (
	"math"
	"testing"
)

func TestADPCMRoundTrip(t *testing.T) {
	// 1 kHz tone at 16 kHz, split into 20 ms frames like the call path does
	var st adpcmState
	var maxErr float64
	for f := 0; f < 10; f++ {
		in := make([]int16, 320)
		for i := range in {
			n := f*320 + i
			in[i] = int16(12000 * math.Sin(2*math.Pi*1000*float64(n)/16000))
		}
		frame := adpcmEncode(&st, in)
		if len(frame) != 164 {
			t.Fatalf("frame size %d", len(frame))
		}
		out := adpcmDecode(frame)
		if len(out) != len(in) {
			t.Fatalf("decoded %d samples", len(out))
		}
		if f == 0 {
			continue // let the step size adapt first
		}
		for i := range in {
			maxErr = math.Max(maxErr, math.Abs(float64(in[i])-float64(out[i])))
		}
	}
	if maxErr > 1500 {
		t.Fatalf("ADPCM error too large: %v", maxErr)
	}
}

func TestADPCMRejectsGarbage(t *testing.T) {
	if adpcmDecode([]byte{1, 2}) != nil || adpcmDecode([]byte{0, 0, 200, 0, 0}) != nil {
		t.Fatal("malformed frames must be rejected")
	}
}
