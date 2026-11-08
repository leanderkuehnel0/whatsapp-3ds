// Call audio: microphone capture (MIC service) and playback/ringtones (DSP via ndsp).
#include "app.h"

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CH_VOICE 0
#define CH_RING  1

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
	make_ringtone();
	setup_channel(CH_VOICE, 1.0f);
	setup_channel(CH_RING, 0.8f);
	dsp_ok = true;
	return true;
}

void audio_exit(void)
{
	audio_call_end();
	if (!dsp_ok)
		return;
	ndspChnWaveBufClear(CH_VOICE);
	ndspChnWaveBufClear(CH_RING);
	ndspExit();
	linearFree(wdata);
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

bool audio_call_begin(void)
{
	audio_ring(0);
	if (mic_on)
		return true;
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
	mic_on = true;
	return true;
}

void audio_call_end(void)
{
	audio_ring(0);
	if (dsp_ok)
		ndspChnWaveBufClear(CH_VOICE);
	if (mic_on) {
		MICU_StopSampling();
		micExit();
		free(micbuf);
		micbuf = NULL;
		mic_on = false;
	}
	level = 0;
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
	if (!mic_on)
		return;
	u32 pos = micGetLastSampleOffset();
	if (pos >= mic_datasize)
		return;
	int guard = 0;
	while (mic_pos != pos && guard++ < 16384) {
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
