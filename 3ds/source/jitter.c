// Adaptive jitter buffer for incoming call audio.
//
// Call audio arrives over WhatsApp's relay, the bridge and the 3DS's WiFi, so it comes in
// bursts (the other phone sends 60 ms packets; TCP and WiFi add their own clumping). Frames
// go into a PCM ring here and are played out at a steady rate. After the buffer runs dry it
// refills to a target level before playing again; the target grows when dry spells look like
// network jitter and shrinks slowly while things are smooth. Pure C, no 3DS calls, so it can
// be tested on a PC.
#include "jitter.h"

#include <string.h>

#define MS(x) ((x) * JB_RATE / 1000)

void jb_reset(Jitter *j)
{
	memset(j, 0, sizeof *j);
	j->target_ms = JB_START_MS;
	j->buffering = true;
}

static void drop_oldest(Jitter *j, int n)
{
	if (n > j->len)
		n = j->len;
	j->head = (j->head + n) % JB_CAP;
	j->len -= n;
}

void jb_push(Jitter *j, const int16_t *pcm, int n, uint64_t now)
{
	if (n <= 0)
		return;
	if (j->buffering && j->dry_since && !j->resumed) {
		// First audio after running dry. A short gap means the network stalled mid-speech:
		// keep more in reserve. A long one is just the other side being silent (phones send
		// nothing while nobody talks), so it says nothing about jitter.
		j->resumed = true;
		if (now - j->dry_since < JB_JITTER_GAP_MS && j->target_ms < JB_MAX_MS) {
			j->target_ms += JB_STEP_MS;
			if (j->target_ms > JB_MAX_MS)
				j->target_ms = JB_MAX_MS;
		}
		j->smooth_since = now;
	}
	if (j->len + n > JB_CAP) { // hopelessly behind: forget the oldest audio
		int over = j->len + n - JB_CAP;
		drop_oldest(j, over);
		j->late += over;
	}
	for (int i = 0; i < n; i++)
		j->ring[(j->head + j->len + i) % JB_CAP] = pcm[i];
	j->len += n;
	if (j->buffering && j->len >= MS(j->target_ms)) {
		j->buffering = false;
		j->fade_in = true;
		j->dry_since = 0;
		j->resumed = false;
	}
}

int jb_pop(Jitter *j, int16_t *out, int n, int ahead, uint64_t now)
{
	if (j->buffering)
		return 0;
	if (j->len < n) {
		if (ahead > 0)
			return 0; // still playing what was queued; more may arrive in time
		// ran dry: refill to the target before playing again
		j->buffering = true;
		j->dry_since = now;
		j->resumed = false;
		j->underruns++;
		return 0;
	}

	if (!j->smooth_since)
		j->smooth_since = now;
	// Smooth for a while: try a smaller reserve (less delay).
	if (now - j->smooth_since > JB_DECAY_AFTER_MS) {
		if (j->target_ms > JB_MIN_MS)
			j->target_ms -= JB_STEP_MS / 2;
		j->smooth_since = now;
	}
	// Far more queued than needed (a burst, or clock drift): squeeze two frames into one to
	// cut the delay. They are blended into each other, which is much less audible than
	// cutting a frame out.
	bool squeeze = false;
	if (j->len > MS(j->target_ms + JB_EXCESS_MS)) {
		if (!j->excess_since)
			j->excess_since = now;
		else if (now - j->excess_since > JB_EXCESS_HOLD_MS && j->len >= 2 * n) {
			squeeze = true;
			j->excess_since = now;
		}
	} else
		j->excess_since = 0;

	if (squeeze) {
		for (int i = 0; i < n; i++) {
			int a = j->ring[(j->head + i) % JB_CAP];
			int b = j->ring[(j->head + n + i) % JB_CAP];
			out[i] = (int16_t)((a * (n - i) + b * i) / n);
		}
		drop_oldest(j, 2 * n);
		j->late += n;
	} else {
		for (int i = 0; i < n; i++)
			out[i] = j->ring[(j->head + i) % JB_CAP];
		drop_oldest(j, n);
	}
	if (j->fade_in) {
		int ramp = MS(JB_FADE_MS) < n ? MS(JB_FADE_MS) : n;
		for (int i = 0; i < ramp; i++)
			out[i] = (int16_t)(out[i] * i / ramp);
		j->fade_in = false;
	}
	return n;
}

int jb_buffered_ms(const Jitter *j) { return j->len * 1000 / JB_RATE; }
