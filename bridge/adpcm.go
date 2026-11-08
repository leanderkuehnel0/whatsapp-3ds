package main

import "encoding/binary"

// IMA ADPCM, used for call audio between the bridge and the 3DS (4 bits per sample).
//
// Frame layout: int16 LE predictor, uint8 step index, uint8 reserved, then
// (len-4)*2 samples as 4-bit codes, low nibble first. Every frame carries its
// starting state, so a dropped frame doesn't corrupt the following ones.

var adpcmSteps = [89]int32{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}

var adpcmIndexAdj = [16]int32{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}

type adpcmState struct {
	pred  int32
	index int32
}

func (s *adpcmState) step(code uint8) int16 {
	step := adpcmSteps[s.index]
	diff := step >> 3
	if code&1 != 0 {
		diff += step >> 2
	}
	if code&2 != 0 {
		diff += step >> 1
	}
	if code&4 != 0 {
		diff += step
	}
	if code&8 != 0 {
		s.pred -= diff
	} else {
		s.pred += diff
	}
	s.pred = max(-32768, min(32767, s.pred))
	s.index = max(0, min(88, s.index+adpcmIndexAdj[code]))
	return int16(s.pred)
}

func (s *adpcmState) encodeSample(x int16) uint8 {
	step := adpcmSteps[s.index]
	diff := int32(x) - s.pred
	var code uint8
	if diff < 0 {
		code = 8
		diff = -diff
	}
	if diff >= step {
		code |= 4
		diff -= step
	}
	if diff >= step>>1 {
		code |= 2
		diff -= step >> 1
	}
	if diff >= step>>2 {
		code |= 1
	}
	s.step(code) // keep the encoder's state identical to the decoder's
	return code
}

// adpcmEncode encodes samples (even count) into one frame, continuing from st.
func adpcmEncode(st *adpcmState, samples []int16) []byte {
	out := make([]byte, 4+len(samples)/2)
	binary.LittleEndian.PutUint16(out[0:], uint16(int16(st.pred)))
	out[2] = byte(st.index)
	for i := 0; i+1 < len(samples); i += 2 {
		lo := st.encodeSample(samples[i])
		hi := st.encodeSample(samples[i+1])
		out[4+i/2] = lo | hi<<4
	}
	return out
}

// adpcmDecode decodes one frame. It returns nil for malformed frames.
func adpcmDecode(frame []byte) []int16 {
	if len(frame) < 4 || frame[2] > 88 {
		return nil
	}
	st := adpcmState{pred: int32(int16(binary.LittleEndian.Uint16(frame))), index: int32(frame[2])}
	out := make([]int16, 0, (len(frame)-4)*2)
	for _, b := range frame[4:] {
		out = append(out, st.step(b&15), st.step(b>>4))
	}
	return out
}
