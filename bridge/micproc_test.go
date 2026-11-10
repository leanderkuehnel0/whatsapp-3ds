package main

import (
	"math"
	"math/rand"
	"testing"
)

// talk makes n frames of a speech-like signal (a buzzy 140 Hz voice whose loudness rises
// and falls a few times a second) or of silence, on top of mic noise and a DC offset.
func micTalk(r *rand.Rand, phase *float64, frames int, voice, noise, dc float64) [][]int16 {
	out := make([][]int16, frames)
	for f := range out {
		fr := make([]int16, callFrameSamples)
		for i := range fr {
			*phase += 1.0 / callSampleRate
			t := *phase
			env := 0.5 + 0.5*math.Sin(2*math.Pi*3*t)
			v := voice * env * (math.Sin(2*math.Pi*140*t) + 0.5*math.Sin(2*math.Pi*420*t) + 0.3*math.Sin(2*math.Pi*1400*t)) / 1.8
			x := v + noise*(r.Float64()*2-1) + dc
			fr[i] = int16(max(-32768, min(32767, x*32768)))
		}
		out[f] = fr
	}
	return out
}

func micStats(frames [][]float32) (peak, rms, mean float64) {
	var sum, sq float64
	n := 0
	for _, f := range frames {
		for _, s := range f {
			v := float64(s)
			peak = max(peak, math.Abs(v))
			sum += v
			sq += v * v
			n++
		}
	}
	return peak, math.Sqrt(sq / float64(n)), sum / float64(n)
}

func micRun(p *micProcessor, in [][]int16) [][]float32 {
	out := make([][]float32, len(in))
	for i, f := range in {
		out[i] = p.process(f)
	}
	return out
}

func TestMicProcessorRaisesQuietSpeech(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	var ph float64
	p := newMicProcessor()
	micRun(p, micTalk(r, &ph, 50, 0, 0.002, 0.05))            // 1 s of room noise first
	micRun(p, micTalk(r, &ph, 250, 0.08, 0.002, 0.05))        // 5 s for the gain to settle
	out := micRun(p, micTalk(r, &ph, 150, 0.08, 0.002, 0.05)) // measured
	peak, _, mean := micStats(out)
	if peak < 0.3 || peak > 0.8 {
		t.Errorf("quiet speech (peak 0.08) came out with peak %.3f, want around %.1f", peak, micTargetPeak)
	}
	if math.Abs(mean) > 0.002 {
		t.Errorf("DC offset 0.05 not removed: mean %.4f", mean)
	}
}

func TestMicProcessorDoesNotPumpNoise(t *testing.T) {
	r := rand.New(rand.NewSource(2))
	var ph float64
	p := newMicProcessor()
	micRun(p, micTalk(r, &ph, 250, 0.08, 0.003, 0))
	gainAfterSpeech := p.gain
	out := micRun(p, micTalk(r, &ph, 1500, 0, 0.003, 0)) // 30 s of nobody talking
	if p.gain > gainAfterSpeech*1.05 {
		t.Errorf("gain rose from %.2f to %.2f during silence", gainAfterSpeech, p.gain)
	}
	if peak, _, _ := micStats(out[1000:]); peak > 0.03 {
		t.Errorf("background noise came out with peak %.3f", peak)
	}
	// noise alone, from the start of a call, must not be amplified at all
	p = newMicProcessor()
	micRun(p, micTalk(r, &ph, 1500, 0, 0.003, 0))
	if p.gain != 1 {
		t.Errorf("gain moved to %.2f on noise alone", p.gain)
	}
}

func TestMicProcessorLoudInputStaysClean(t *testing.T) {
	r := rand.New(rand.NewSource(3))
	var ph float64
	p := newMicProcessor()
	micRun(p, micTalk(r, &ph, 250, 0.08, 0.002, 0))       // settle on quiet speech: high gain
	out := micRun(p, micTalk(r, &ph, 100, 0.9, 0.002, 0)) // then someone shouts
	if peak, _, _ := micStats(out); peak > 1 {
		t.Errorf("loud input went past full scale: peak %.3f", peak)
	}
	if peak, _, _ := micStats(out[50:]); peak > 0.85 {
		t.Errorf("gain did not come down for loud speech: peak %.3f", peak)
	}
	// speech that is already at a good level passes nearly unchanged
	p = newMicProcessor()
	in := micTalk(r, &ph, 300, 0.5, 0.002, 0)
	out = micRun(p, in)
	if p.gain < 0.8 || p.gain > 1.6 {
		t.Errorf("gain for normal-level speech is %.2f, want about 1", p.gain)
	}
}

func TestSoftLimit(t *testing.T) {
	for _, x := range []float64{0, 0.3, -0.79, 0.8} {
		if softLimit(x) != x {
			t.Errorf("softLimit(%v) = %v, want unchanged", x, softLimit(x))
		}
	}
	prev := 0.8
	for x := 0.81; x < 6; x += 0.05 {
		y := softLimit(x)
		if y < prev || y <= prev && x < 2 || y > 1 {
			t.Fatalf("softLimit(%v) = %v (previous %v)", x, y, prev)
		}
		if softLimit(-x) != -y {
			t.Fatalf("softLimit not symmetric at %v", x)
		}
		prev = y
	}
}
