// IMA ADPCM for call audio. Must stay bit-identical to bridge/adpcm.go (see PROTOCOL.md).
#include "app.h"

static const int steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int index_adj[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static s16 step(AdpcmState *st, int code)
{
	int s = steps[st->index];
	int diff = s >> 3;
	if (code & 1)
		diff += s >> 2;
	if (code & 2)
		diff += s >> 1;
	if (code & 4)
		diff += s;
	st->pred += (code & 8) ? -diff : diff;
	if (st->pred > 32767)
		st->pred = 32767;
	if (st->pred < -32768)
		st->pred = -32768;
	st->index += index_adj[code];
	if (st->index < 0)
		st->index = 0;
	if (st->index > 88)
		st->index = 88;
	return (s16)st->pred;
}

static int encode_sample(AdpcmState *st, s16 x)
{
	int s = steps[st->index];
	int diff = x - st->pred;
	int code = 0;
	if (diff < 0) {
		code = 8;
		diff = -diff;
	}
	if (diff >= s) {
		code |= 4;
		diff -= s;
	}
	if (diff >= s >> 1) {
		code |= 2;
		diff -= s >> 1;
	}
	if (diff >= s >> 2)
		code |= 1;
	step(st, code);
	return code;
}

size_t adpcm_encode(AdpcmState *st, const s16 *in, int n, unsigned char *out)
{
	u16 p = (u16)(s16)st->pred;
	out[0] = p & 0xFF;
	out[1] = p >> 8;
	out[2] = (unsigned char)st->index;
	out[3] = 0;
	for (int i = 0; i + 1 < n; i += 2) {
		int lo = encode_sample(st, in[i]);
		int hi = encode_sample(st, in[i + 1]);
		out[4 + i / 2] = (unsigned char)(lo | (hi << 4));
	}
	return 4 + n / 2;
}

int adpcm_decode(const unsigned char *frame, size_t len, s16 *out, int max)
{
	if (len < 4 || frame[2] > 88)
		return 0;
	AdpcmState st = {(s16)(frame[0] | (frame[1] << 8)), frame[2]};
	int n = 0;
	for (size_t i = 4; i < len && n + 1 < max; i++) {
		out[n++] = step(&st, frame[i] & 15);
		out[n++] = step(&st, frame[i] >> 4);
	}
	return n;
}
