package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"

	"github.com/kazzmir/opus-go/ogg"
	"github.com/kazzmir/opus-go/opus"
)

// Voice messages: the 3DS records and plays 16 kHz mono ADPCM frames (see adpcm.go);
// WhatsApp voice notes are Ogg/Opus. These helpers convert between the two in pure Go.

const (
	voiceRate       = 16000
	voiceFrameMS    = 20
	voiceFrame      = voiceRate * voiceFrameMS / 1000 // 320 samples
	adpcmFrameBytes = 4 + callFrameSamples/2          // 164
	maxVoiceSeconds = 300                             // longer messages are cut for the 3DS
	voiceBitrate    = 24000
	waveformBuckets = 64
)

// adpcmFramesToPCM decodes concatenated 164-byte ADPCM frames.
func adpcmFramesToPCM(data []byte) ([]int16, error) {
	if len(data) == 0 || len(data)%adpcmFrameBytes != 0 {
		return nil, fmt.Errorf("voice data must be a multiple of %d bytes", adpcmFrameBytes)
	}
	pcm := make([]int16, 0, len(data)/adpcmFrameBytes*callFrameSamples)
	for off := 0; off < len(data); off += adpcmFrameBytes {
		s := adpcmDecode(data[off : off+adpcmFrameBytes])
		if s == nil {
			return nil, errors.New("malformed ADPCM frame")
		}
		pcm = append(pcm, s...)
	}
	return pcm, nil
}

// pcmToADPCMFrames encodes PCM as self-contained 20 ms ADPCM frames, zero-padding the last one.
func pcmToADPCMFrames(pcm []int16) []byte {
	var st adpcmState
	out := make([]byte, 0, (len(pcm)/callFrameSamples+1)*adpcmFrameBytes)
	frame := make([]int16, callFrameSamples)
	for off := 0; off < len(pcm); off += callFrameSamples {
		n := copy(frame, pcm[off:])
		clear(frame[n:])
		out = append(out, adpcmEncode(&st, frame)...)
	}
	return out
}

// encodeOggOpus encodes 16 kHz mono PCM as an Ogg/Opus voice note.
func encodeOggOpus(pcm []int16) ([]byte, error) {
	enc, err := opus.NewEncoder(voiceRate, 1, opus.ApplicationVoIP)
	if err != nil {
		return nil, err
	}
	defer enc.Close()
	if err := enc.SetBitrate(voiceBitrate); err != nil {
		return nil, err
	}
	lookahead, err := enc.Lookahead()
	if err != nil {
		return nil, err
	}
	preSkip, err := enc.PreSkip()
	if err != nil {
		return nil, err
	}

	var buf bytes.Buffer
	pw := ogg.NewPageWriter(&buf, 0x57413344) // "WA3D"
	head, err := ogg.BuildOpusHeadPacket(ogg.OpusHead{
		Version: 1, Channels: 1, PreSkip: uint16(preSkip), InputSampleRate: voiceRate,
	})
	if err != nil {
		return nil, err
	}
	tags, err := ogg.BuildOpusTagsPacket(ogg.OpusTags{Vendor: "wa3ds"})
	if err != nil {
		return nil, err
	}
	if err := pw.WriteHeaderPacket(head, true); err != nil {
		return nil, err
	}
	if err := pw.WriteHeaderPacket(tags, false); err != nil {
		return nil, err
	}

	// Granules count 48 kHz samples. Feed silence after the input until the encoder's
	// lookahead is flushed; the last packet carries EOS and the exact end position.
	const scale = 48000 / voiceRate
	frame := make([]int16, voiceFrame)
	packet := make([]byte, 4000)
	var held []byte
	var heldGranule uint64
	fed := 0
	for fed < len(pcm)+lookahead {
		n := 0
		if fed < len(pcm) {
			n = copy(frame, pcm[fed:])
		}
		clear(frame[n:])
		nb, err := enc.Encode(frame, voiceFrame, packet)
		if err != nil {
			return nil, err
		}
		fed += voiceFrame
		if held != nil {
			if err := pw.WritePacket(held, heldGranule, false); err != nil {
				return nil, err
			}
		}
		held = append(held[:0], packet[:nb]...)
		heldGranule = uint64(fed * scale)
	}
	if err := pw.WritePacket(held, uint64(preSkip+len(pcm)*scale), true); err != nil {
		return nil, err
	}
	if err := pw.Flush(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// decodeOggOpus decodes an Ogg/Opus file to 16 kHz mono PCM, keeping at most maxSamples.
func decodeOggOpus(data []byte, maxSamples int) ([]int16, error) {
	r, err := ogg.NewOpusReader(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	dec, err := opus.NewDecoder(voiceRate, 1) // Opus decodes to any rate/channel count directly
	if err != nil {
		return nil, err
	}
	defer dec.Close()

	skip := int(r.Head.PreSkip) / (48000 / voiceRate)
	out := make([]int16, 0, voiceRate*30)
	frame := make([]int16, voiceRate*120/1000) // largest Opus packet: 120 ms
	for len(out) < maxSamples {
		pkt, err := r.ReadAudioPacket()
		if errors.Is(err, io.EOF) {
			break
		} else if err != nil {
			if len(out) > 0 {
				break // keep what decoded fine from a truncated file
			}
			return nil, err
		}
		n, err := dec.Decode(pkt.Data, frame, len(frame), false)
		if err != nil {
			continue
		}
		s := frame[:n]
		if skip > 0 {
			d := min(skip, len(s))
			s, skip = s[d:], skip-d
		}
		out = append(out, s...)
	}
	return out[:min(len(out), maxSamples)], nil
}

// waveform returns WhatsApp's 64-value (0-100) loudness overview of a voice note.
func waveform(pcm []int16) []byte {
	out := make([]byte, waveformBuckets)
	if len(pcm) == 0 {
		return out
	}
	var peak float64
	rms := make([]float64, waveformBuckets)
	for i := range rms {
		a, b := i*len(pcm)/waveformBuckets, (i+1)*len(pcm)/waveformBuckets
		var sum float64
		for _, s := range pcm[a:max(b, a+1)] {
			sum += float64(s) * float64(s)
		}
		rms[i] = math.Sqrt(sum / float64(max(1, b-a)))
		peak = max(peak, rms[i])
	}
	for i, v := range rms {
		if peak > 0 {
			out[i] = byte(math.Round(100 * v / peak))
		}
	}
	return out
}
