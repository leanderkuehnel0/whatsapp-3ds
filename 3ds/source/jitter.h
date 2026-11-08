// Adaptive jitter buffer for incoming call audio (see jitter.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define JB_RATE            16000
#define JB_CAP             (JB_RATE * 8 / 10) // 800 ms of audio at most
#ifndef JB_START_MS
#define JB_START_MS        120  // reserve built up before playback starts
#endif
#ifndef JB_MIN_MS
#define JB_MIN_MS          60
#endif
#ifndef JB_MAX_MS
#define JB_MAX_MS          300
#endif
#ifndef JB_STEP_MS
#define JB_STEP_MS         40   // added after a stall mid-speech
#endif
#ifndef JB_JITTER_GAP_MS
#define JB_JITTER_GAP_MS   600  // dry spells shorter than this count as network jitter
#endif
#ifndef JB_DECAY_AFTER_MS
#define JB_DECAY_AFTER_MS  4000 // smooth this long: lower the reserve a bit
#endif
#ifndef JB_EXCESS_MS
#define JB_EXCESS_MS       120  // queued beyond target + this ...
#endif
#ifndef JB_EXCESS_HOLD_MS
#define JB_EXCESS_HOLD_MS  500  // ... for this long: skip a frame
#endif

typedef struct {
	int16_t  ring[JB_CAP];
	int      head, len;      // samples
	bool     buffering;
	bool     resumed;        // audio arrived again after running dry
	int      target_ms;
	uint64_t dry_since, smooth_since, excess_since;
	uint32_t underruns;      // times playback ran dry
	uint32_t late;           // samples discarded to keep the delay down
} Jitter;

void jb_reset(Jitter *j);
void jb_push(Jitter *j, const int16_t *pcm, int n, uint64_t now_ms);
// Takes n samples for playback, or returns 0 when there aren't enough yet. ahead is how many
// frames the DSP still has queued; running dry only counts once that reaches 0.
int  jb_pop(Jitter *j, int16_t *out, int n, int ahead, uint64_t now_ms);
int  jb_buffered_ms(const Jitter *j);
