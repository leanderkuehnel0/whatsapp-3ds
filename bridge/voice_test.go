package main

import (
	"math"
	"testing"
)

func tone(seconds float64) []int16 {
	n := int(seconds * voiceRate)
	pcm := make([]int16, n)
	for i := range pcm {
		// a 300 Hz tone with a slow volume swell, like speech-ish energy
		amp := 8000 * (0.5 + 0.5*math.Sin(2*math.Pi*0.5*float64(i)/voiceRate))
		pcm[i] = int16(amp * math.Sin(2*math.Pi*300*float64(i)/voiceRate))
	}
	return pcm
}

func TestVoiceRoundTrip(t *testing.T) {
	in := tone(2.5)
	frames := pcmToADPCMFrames(in)
	if len(frames)%adpcmFrameBytes != 0 {
		t.Fatalf("frames not aligned: %d", len(frames))
	}
	pcm, err := adpcmFramesToPCM(frames)
	if err != nil {
		t.Fatal(err)
	}
	ogg, err := encodeOggOpus(pcm)
	if err != nil {
		t.Fatal(err)
	}
	if string(ogg[:4]) != "OggS" {
		t.Fatal("not an Ogg stream")
	}
	t.Logf("2.5 s voice note: %d bytes Ogg/Opus", len(ogg))

	out, err := decodeOggOpus(ogg, voiceRate*maxVoiceSeconds)
	if err != nil {
		t.Fatal(err)
	}
	// duration preserved within one frame (pre-skip trimmed)
	if d := len(out) - len(pcm); d < -voiceFrame || d > voiceFrame {
		t.Fatalf("decoded %d samples, want ~%d", len(out), len(pcm))
	}
	// energy roughly preserved (lossy codec, so loose bounds)
	energy := func(s []int16) float64 {
		var e float64
		for _, v := range s {
			e += float64(v) * float64(v)
		}
		return math.Sqrt(e / float64(len(s)))
	}
	ratio := energy(out) / energy(pcm)
	if ratio < 0.6 || ratio > 1.4 {
		t.Fatalf("energy ratio %.2f after Opus round trip", ratio)
	}

	w := waveform(pcm)
	if len(w) != waveformBuckets {
		t.Fatalf("waveform length %d", len(w))
	}
	var top byte
	for _, v := range w {
		top = max(top, v)
	}
	if top != 100 {
		t.Fatalf("waveform not normalised, max %d", top)
	}
}

func TestDecodeLimit(t *testing.T) {
	ogg, err := encodeOggOpus(tone(3))
	if err != nil {
		t.Fatal(err)
	}
	out, err := decodeOggOpus(ogg, voiceRate)
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != voiceRate {
		t.Fatalf("limit not applied: %d samples", len(out))
	}
}

func TestRejectsBadVoiceData(t *testing.T) {
	if _, err := adpcmFramesToPCM(make([]byte, 100)); err == nil {
		t.Fatal("misaligned voice data must be rejected")
	}
	if _, err := decodeOggOpus([]byte("not ogg"), voiceRate); err == nil {
		t.Fatal("garbage must be rejected")
	}
}
