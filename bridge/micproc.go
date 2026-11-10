package main

import "math"

// micProcessor cleans up the 3DS microphone before a call's audio is encoded for WhatsApp.
//
// The console's microphone is quiet, noisy and carries low rumble (handling noise, a DC
// offset). The speech codec spends bits on all of that, and the other person has to turn
// their volume up. Three steps, each one 20 ms frame at a time:
//
//  1. a high-pass filter removes everything below ~120 Hz,
//  2. a slow automatic gain brings speech up to a normal level. It learns the level from
//     frames that are clearly speech and holds still during pauses, so it does not pump the
//     background noise up between sentences,
//  3. a soft limiter keeps sudden loud sounds from clipping.
type micProcessor struct {
	b0, b1, b2, a1, a2 float64 // high-pass biquad
	x1, x2, y1, y2     float64

	noise float64 // RMS of the quietest recent frames
	level float64 // how loud speech peaks are; 0 until speech has been heard
	gain  float64
}

const (
	micHighPassHz  = 120.0
	micTargetPeak  = 0.5 // speech peaks are brought to about -6 dBFS
	micMaxGain     = 6.0 // +15.6 dB at most, so a silent room is not turned into hiss
	micMinGain     = 0.5
	micSpeechRatio = 3.0  // a frame this far above the noise floor counts as speech
	micSpeechPeak  = 0.01 // ... if it is also at least this loud
	micNoiseMax    = 0.02
	micLimitKnee   = 0.8
)

func newMicProcessor() *micProcessor {
	// second-order Butterworth high-pass
	w := 2 * math.Pi * micHighPassHz / callSampleRate
	cos, alpha := math.Cos(w), math.Sin(w)/math.Sqrt2
	a0 := 1 + alpha
	return &micProcessor{
		b0: (1 + cos) / 2 / a0, b1: -(1 + cos) / a0, b2: (1 + cos) / 2 / a0,
		a1: -2 * cos / a0, a2: (1 - alpha) / a0,
		noise: 0.003, gain: 1,
	}
}

// process turns one frame of microphone samples into float PCM ready for the encoder.
func (p *micProcessor) process(in []int16) []float32 {
	out := make([]float32, len(in))
	if len(in) == 0 {
		return out
	}
	buf := make([]float64, len(in))
	var peak, sum float64
	for i, s := range in {
		x := float64(s) / 32768
		y := p.b0*x + p.b1*p.x1 + p.b2*p.x2 - p.a1*p.y1 - p.a2*p.y2
		p.x2, p.x1 = p.x1, x
		p.y2, p.y1 = p.y1, y
		buf[i] = y
		peak = max(peak, math.Abs(y))
		sum += y * y
	}
	rms := math.Sqrt(sum / float64(len(in)))

	// The noise floor follows the quietest frames down at once and creeps up slowly.
	if rms < p.noise {
		p.noise = rms
	} else {
		p.noise = min(p.noise*1.002, micNoiseMax)
	}
	p.noise = max(p.noise, 1e-5)

	want := p.gain
	if rms > micSpeechRatio*p.noise && peak > micSpeechPeak {
		if peak > p.level {
			p.level += (peak - p.level) * 0.5 // a single pop should not set the level
		} else {
			p.level *= 0.999 // drifts down over ~15 s of quieter speech
		}
		want = min(max(micTargetPeak/p.level, micMinGain), micMaxGain)
	}
	// Turn down quickly, up slowly.
	next := p.gain
	if want < next {
		next += (want - next) * 0.3
	} else {
		next += (want - next) * 0.03
	}

	step := (next - p.gain) / float64(len(in))
	g := p.gain
	for i, y := range buf {
		g += step
		out[i] = float32(softLimit(y * g))
	}
	p.gain = next
	return out
}

// softLimit leaves samples below the knee untouched and bends louder ones towards ±1.
func softLimit(x float64) float64 {
	a := math.Abs(x)
	if a <= micLimitKnee {
		return x
	}
	a = micLimitKnee + (1-micLimitKnee)*math.Tanh((a-micLimitKnee)/(1-micLimitKnee))
	return math.Copysign(a, x)
}
