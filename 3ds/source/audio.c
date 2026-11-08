// Audio: microphone capture (calls and voice-message recording), call playback, ringtones,
// and a streaming player for voice messages. Uses the MIC service and the DSP via ndsp.
#include "app.h"

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CH_VOICE 0
#define CH_RING  1
#define CH_CLIP  2

// playback ring of 20 ms buffers
#define NBUF        16
#define MAX_QUEUED  8   // ~160 ms; newer audio is dropped beyond this to keep latency down
#define PREBUFFER   2   // silent buffers queued after an underrun to absorb network jitter

static bool       dsp_ok;
static ndspWaveBuf wbuf[NBUF];
static s16       *wdata;    // NBUF * CALL_FRAME samples, linear memory
static ndspWaveBuf ringbuf;
static s16       *ringdata;
static int        ring_kind;

// microphone
#define MIC_BUF_SIZE 0x30000
static u8   *micbuf;
static u32   mic_datasize, mic_pos;
static bool  mic_on;
static float mic_phase;     // resampler position between the last two input samples
static s16   mic_prev;
static s16   frame[CALL_FRAME];
static int   frame_n;
static AdpcmState enc;
static float level;

typedef enum { MIC_OFF, MIC_CALL, MIC_RECORD } MicMode;
static MicMode mic_mode;

// voice-message recording: ADPCM frames, same format the bridge accepts
#define REC_MAX_FRAMES (120 * 1000 / 20) // 2 minutes
static unsigned char *rec;
static size_t rec_len, rec_cap;
static AdpcmState rec_enc;

// voice-message player: decodes ADPCM frames into a few rotating half-second buffers
#define CLIP_BUFS       4
#define CLIP_BUF_FRAMES 25
#define CLIP_BUF_SAMPLES (CLIP_BUF_FRAMES * CALL_FRAME)
static ndspWaveBuf cbuf[CLIP_BUFS];
static s16        *cdata;
static unsigned char *clip;      // owned
static size_t      clip_frames, clip_next;
static size_t      clip_played;  // samples in buffers that finished playing
static bool        clip_queued[CLIP_BUFS];
static bool        clip_on;

bool audio_available(void) { return dsp_ok; }

static void make_ringtone(void)
{
	// 3 s loop: two 0.4 s bursts of 440+480 Hz, then silence
	const int n = CALL_RATE * 3;
	ringdata = linearAlloc(n * sizeof(s16));
	if (!ringdata)
		return;
	for (int i = 0; i < n; i++) {
		float t = (float)i / CALL_RATE;
		bool on = (t < 0.4f) || (t >= 0.6f && t < 1.0f);
		float v = on ? 0.25f * (sinf(2 * M_PI * 440 * t) + sinf(2 * M_PI * 480 * t)) : 0;
		ringdata[i] = (s16)(v * 32767);
	}
	DSP_FlushDataCache(ringdata, n * sizeof(s16));
	memset(&ringbuf, 0, sizeof ringbuf);
	ringbuf.data_vaddr = ringdata;
	ringbuf.nsamples = n;
	ringbuf.looping = true;
}

static void setup_channel(int ch, float vol)
{
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(ch, CALL_RATE);
	ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	float mix[12] = {0};
	mix[0] = mix[1] = vol;
	ndspChnSetMix(ch, mix);
}

bool audio_init(void)
{
	// fails when sdmc:/3ds/dspfirm.cdc is missing (dump it once with DSP1)
	if (R_FAILED(ndspInit()))
		return false;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	wdata = linearAlloc(NBUF * CALL_FRAME * sizeof(s16));
	if (!wdata) {
		ndspExit();
		return false;
	}
	memset(wbuf, 0, sizeof wbuf);
	for (int i = 0; i < NBUF; i++) {
		wbuf[i].data_vaddr = wdata + i * CALL_FRAME;
		wbuf[i].status = NDSP_WBUF_DONE;
	}
	cdata = linearAlloc(CLIP_BUFS * CLIP_BUF_SAMPLES * sizeof(s16));
	memset(cbuf, 0, sizeof cbuf);
	for (int i = 0; i < CLIP_BUFS && cdata; i++) {
		cbuf[i].data_vaddr = cdata + i * CLIP_BUF_SAMPLES;
		cbuf[i].status = NDSP_WBUF_DONE;
	}
	make_ringtone();
	setup_channel(CH_VOICE, 1.0f);
	setup_channel(CH_RING, 0.8f);
	setup_channel(CH_CLIP, 1.0f);
	dsp_ok = true;
	return true;
}

void audio_exit(void)
{
	audio_call_end();
	audio_record_discard();
	audio_clip_stop();
	if (!dsp_ok)
		return;
	ndspChnWaveBufClear(CH_VOICE);
	ndspChnWaveBufClear(CH_RING);
	ndspChnWaveBufClear(CH_CLIP);
	ndspExit();
	linearFree(wdata);
	if (cdata)
		linearFree(cdata);
	if (ringdata)
		linearFree(ringdata);
	dsp_ok = false;
}

void audio_ring(int kind)
{
	if (!dsp_ok || !ringdata || kind == ring_kind)
		return;
	ndspChnWaveBufClear(CH_RING);
	ring_kind = kind;
	if (kind) {
		float mix[12] = {0};
		mix[0] = mix[1] = kind == 1 ? 0.9f : 0.35f; // ringback is quieter
		ndspChnSetMix(CH_RING, mix);
		ringbuf.status = NDSP_WBUF_FREE;
		ndspChnWaveBufAdd(CH_RING, &ringbuf);
	}
}

static bool mic_start(MicMode mode)
{
	if (mic_on)
		return mic_mode == mode;
	micbuf = memalign(0x1000, MIC_BUF_SIZE);
	if (!micbuf)
		return false;
	if (R_FAILED(micInit(micbuf, MIC_BUF_SIZE))) {
		free(micbuf);
		micbuf = NULL;
		return false;
	}
	mic_datasize = micGetSampleDataSize();
	MICU_SetAllowShellClosed(true);
	if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, mic_datasize, true))) {
		micExit();
		free(micbuf);
		micbuf = NULL;
		return false;
	}
	mic_pos = micGetLastSampleOffset();
	mic_phase = 0;
	mic_prev = 0;
	frame_n = 0;
	memset(&enc, 0, sizeof enc);
	mic_mode = mode;
	mic_on = true;
	return true;
}

static void mic_stop(void)
{
	if (mic_on) {
		MICU_StopSampling();
		micExit();
		free(micbuf);
		micbuf = NULL;
		mic_on = false;
	}
	mic_mode = MIC_OFF;
	level = 0;
}

bool audio_call_begin(void)
{
	audio_ring(0);
	audio_clip_stop();
	if (mic_on && mic_mode == MIC_RECORD)
		audio_record_discard(); // an incoming call wins over a half-done recording
	return mic_start(MIC_CALL);
}

void audio_call_end(void)
{
	audio_ring(0);
	if (dsp_ok)
		ndspChnWaveBufClear(CH_VOICE);
	if (mic_mode == MIC_CALL)
		mic_stop();
}

// ---------------------------------------------------------------- recording

bool audio_record_start(void)
{
	if (mic_on)
		return false; // in a call
	audio_clip_stop();
	free(rec);
	rec_cap = 64 * 1024;
	rec = malloc(rec_cap);
	rec_len = 0;
	memset(&rec_enc, 0, sizeof rec_enc);
	if (!rec || !mic_start(MIC_RECORD)) {
		free(rec);
		rec = NULL;
		return false;
	}
	return true;
}

bool audio_recording(void) { return mic_on && mic_mode == MIC_RECORD; }

void audio_record_stop(void)
{
	if (mic_mode == MIC_RECORD)
		mic_stop();
}

const unsigned char *audio_record_data(size_t *len)
{
	*len = rec_len;
	return rec;
}

u32 audio_record_ms(void) { return (u32)(rec_len / (4 + CALL_FRAME / 2)) * 20; }

void audio_record_discard(void)
{
	audio_record_stop();
	free(rec);
	rec = NULL;
	rec_len = rec_cap = 0;
}

static void record_frame(void)
{
	const size_t fb = 4 + CALL_FRAME / 2;
	if (rec_len / fb >= REC_MAX_FRAMES) {
		mic_stop(); // limit reached: stop, keep what we have
		return;
	}
	if (rec_len + fb > rec_cap) {
		size_t cap = rec_cap * 2;
		unsigned char *n = realloc(rec, cap);
		if (!n) {
			mic_stop();
			return;
		}
		rec = n;
		rec_cap = cap;
	}
	rec_len += adpcm_encode(&rec_enc, frame, CALL_FRAME, rec + rec_len);
}

// ---------------------------------------------------------------- voice message player

void audio_clip_stop(void)
{
	if (dsp_ok)
		ndspChnWaveBufClear(CH_CLIP);
	free(clip);
	clip = NULL;
	clip_on = false;
	clip_frames = clip_next = clip_played = 0;
	memset(clip_queued, 0, sizeof clip_queued);
}

bool audio_clip_play(unsigned char *data, size_t len)
{
	audio_clip_stop();
	const size_t fb = 4 + CALL_FRAME / 2;
	if (!dsp_ok || !cdata || len < fb) {
		free(data);
		return false;
	}
	clip = data;
	clip_frames = len / fb;
	clip_on = true;
	return true;
}

bool audio_clip_playing(void) { return clip_on; }

void audio_clip_progress(u32 *pos_ms, u32 *total_ms)
{
	u32 pos = clip_played + (clip_on ? ndspChnGetSamplePos(CH_CLIP) : 0);
	*pos_ms = pos / (CALL_RATE / 1000);
	*total_ms = clip_frames * 20;
	if (*pos_ms > *total_ms)
		*pos_ms = *total_ms;
}

static void clip_update(void)
{
	if (!clip_on)
		return;
	const size_t fb = 4 + CALL_FRAME / 2;
	bool busy = false;
	for (int i = 0; i < CLIP_BUFS; i++) {
		ndspWaveBuf *b = &cbuf[i];
		if (b->status == NDSP_WBUF_QUEUED || b->status == NDSP_WBUF_PLAYING) {
			busy = true;
			continue;
		}
		if (clip_queued[i]) { // this buffer just finished
			clip_played += b->nsamples;
			clip_queued[i] = false;
		}
		if (clip_next >= clip_frames)
			continue;
		s16 *dst = (s16 *)b->data_vaddr;
		int n = 0;
		while (n < CLIP_BUF_SAMPLES && clip_next < clip_frames) {
			n += adpcm_decode(clip + clip_next * fb, fb, dst + n, CLIP_BUF_SAMPLES - n);
			clip_next++;
		}
		b->nsamples = n;
		b->looping = false;
		DSP_FlushDataCache(dst, n * sizeof(s16));
		ndspChnWaveBufAdd(CH_CLIP, b);
		clip_queued[i] = true;
		busy = true;
	}
	if (!busy && clip_next >= clip_frames)
		audio_clip_stop(); // finished
}

float audio_mic_level(void) { return level; }

static void send_frame(void)
{
	// level meter: peak of this frame, decaying smoothly
	int peak = 0;
	for (int i = 0; i < CALL_FRAME; i++) {
		int a = abs(frame[i]);
		if (a > peak)
			peak = a;
	}
	float l = peak / 32768.0f;
	level = l > level ? l : level * 0.85f;

	if (mic_mode == MIC_RECORD) {
		record_frame();
		return;
	}
	if (g_app.call_muted || g_app.call_state != CALL_ACTIVE)
		return;
	unsigned char out[4 + CALL_FRAME / 2];
	size_t n = adpcm_encode(&enc, frame, CALL_FRAME, out);
	net_send_binary("a", "AUD", out, n);
}

// The mic runs at 16360 Hz; resample to 16000 Hz with linear interpolation.
static void push_mic_sample(s16 s)
{
	const float step = 16360.0f / 16000.0f;
	while (mic_phase < 1.0f) {
		float v = mic_prev + (s - mic_prev) * mic_phase;
		frame[frame_n++] = (s16)v;
		if (frame_n == CALL_FRAME) {
			send_frame();
			frame_n = 0;
		}
		mic_phase += step;
	}
	mic_phase -= 1.0f;
	mic_prev = s;
}

void audio_update(void)
{
	clip_update();
	if (!mic_on)
		return;
	u32 pos = micGetLastSampleOffset();
	if (pos >= mic_datasize)
		return;
	int guard = 0;
	while (mic_on && mic_pos != pos && guard++ < 16384) {
		s16 s = *(s16 *)(micbuf + mic_pos);
		mic_pos += 2;
		if (mic_pos >= mic_datasize)
			mic_pos = 0;
		push_mic_sample(s);
	}
}

static int queued(void)
{
	int q = 0;
	for (int i = 0; i < NBUF; i++)
		if (wbuf[i].status == NDSP_WBUF_QUEUED || wbuf[i].status == NDSP_WBUF_PLAYING)
			q++;
	return q;
}

static ndspWaveBuf *free_buf(void)
{
	for (int i = 0; i < NBUF; i++)
		if (wbuf[i].status == NDSP_WBUF_DONE || wbuf[i].status == NDSP_WBUF_FREE)
			return &wbuf[i];
	return NULL;
}

static void enqueue(const s16 *pcm, int n)
{
	ndspWaveBuf *b = free_buf();
	if (!b)
		return;
	s16 *dst = (s16 *)b->data_vaddr;
	if (pcm)
		memcpy(dst, pcm, n * sizeof(s16));
	else
		memset(dst, 0, n * sizeof(s16));
	b->nsamples = n;
	b->looping = false;
	DSP_FlushDataCache(dst, n * sizeof(s16));
	ndspChnWaveBufAdd(CH_VOICE, b);
}

void audio_play_frame(const unsigned char *data, size_t len)
{
	if (!dsp_ok || g_app.call_state != CALL_ACTIVE)
		return;
	s16 pcm[CALL_FRAME];
	int n = adpcm_decode(data, len, pcm, CALL_FRAME);
	if (n <= 0)
		return;
	int q = queued();
	if (q >= MAX_QUEUED)
		return; // too far behind: drop to catch up
	if (q == 0)
		for (int i = 0; i < PREBUFFER; i++)
			enqueue(NULL, CALL_FRAME);
	enqueue(pcm, n);
}
