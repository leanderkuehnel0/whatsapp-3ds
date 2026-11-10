// Camera capture for sending photos: live VGA preview, snapshot, JPEG encoding.
//
// The camera hands over a frame in small pieces that have to be collected without a pause:
// when nobody is receiving while the sensor delivers, its buffer overflows and capture stops
// until it is restarted. A dedicated high-priority thread therefore asks for the next frame
// the moment one is complete, and restarts capture right away after an overflow. The main
// loop only picks up the newest finished frame.
#include "app.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define CAM_W     640
#define CAM_H     480
#define CAM_BYTES (CAM_W * CAM_H * 2)
#define PREV_W    (CAM_W / 2)
#define PREV_H    (CAM_H / 2)
#define TEX_W     512
#define TEX_H     256
#define STALL_NS  1500000000LL // no frame for this long (e.g. the lid was closed): start over
#define WARMUP    6            // frames dropped after switching the sensor on, while exposure
                               // and white balance settle (they start out dark and tinted)

static bool   running, have_frame, frozen, front;
static C3D_Tex tex;
static bool   tex_ok;
static const Tex3DS_SubTexture sub = {PREV_W, PREV_H, 0.0f, 1.0f, (float)PREV_W / TEX_W, 1.0f - (float)PREV_H / TEX_H};

// Three frame buffers rotate between the camera (being filled), the newest finished frame,
// and the one the main thread shows and encodes. Indices are swapped under the lock.
static u16      *bufs[3];
static int       i_fill, i_ready, i_show;
static bool      fresh;        // i_ready holds a frame the main thread hasn't taken yet
static LightLock lock;
static Thread    thread;
static Handle    ev_stop;
static bool      thread_on;

// frames shown per second, for the preview's status line
static u32 fps, fps_count;
static u64 fps_since;

// position of each pixel of an 8x8 texture tile (GPU textures are stored in Morton order)
static u8 morton[64];

static void morton_init(void)
{
	for (u32 y = 0; y < 8; y++)
		for (u32 x = 0; x < 8; x++)
			morton[y * 8 + x] = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// average of two RGB565 pixels, per channel
static inline u16 avg565(u16 a, u16 b)
{
	return (a & b) + (((a ^ b) & 0xF7DE) >> 1);
}

// Downscales a VGA RGB565 frame by 2 into the preview texture.
static void upload_preview(const u16 *src)
{
	if (!tex_ok)
		return;
	u16 *dst = (u16 *)tex.data;
	for (u32 ty = 0; ty < PREV_H / 8; ty++) {
		for (u32 tx = 0; tx < PREV_W / 8; tx++) {
			u16 *tile = dst + ((ty * (TEX_W / 8) + tx) << 6);
			for (u32 y = 0; y < 8; y++) {
				const u16 *r0 = src + (ty * 8 + y) * 2 * CAM_W + tx * 16;
				const u16 *r1 = r0 + CAM_W;
				const u8 *m = morton + y * 8;
				for (u32 x = 0; x < 8; x++)
					tile[m[x]] = avg565(avg565(r0[x * 2], r0[x * 2 + 1]), avg565(r1[x * 2], r1[x * 2 + 1]));
			}
		}
	}
	C3D_TexFlush(&tex);
}

// ---------------------------------------------------------------- capture thread

static void sensor_start(u32 sel)
{
	CAMU_Activate(sel);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);
}

// Waits for the stop request; returns true if it came.
static bool stop_requested(s64 wait_ns)
{
	return svcWaitSynchronization(ev_stop, wait_ns) == 0;
}

static void capture_thread(void *arg)
{
	(void)arg;
	// the inner camera and the outer right camera both deliver on port 1
	u32 sel = front ? SELECT_IN1 : SELECT_OUT1;
	u32 unit = 0;
	Handle ev_err = 0, ev_recv = 0;

	CAMU_SetSize(sel, SIZE_VGA, CONTEXT_A);
	CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(sel, FRAME_RATE_15);
	CAMU_SetNoiseFilter(sel, true);
	CAMU_SetAutoExposure(sel, true);
	CAMU_SetAutoWhiteBalance(sel, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	CAMU_GetMaxBytes(&unit, CAM_W, CAM_H);
	CAMU_SetTransferBytes(PORT_CAM1, unit, CAM_W, CAM_H);
	if (R_FAILED(CAMU_GetBufferErrorInterruptEvent(&ev_err, PORT_CAM1)))
		ev_err = 0;
	sensor_start(sel);

	int warm = 0, overflows = 0;
	for (;;) {
		if (!ev_recv && R_FAILED(CAMU_SetReceiving(&ev_recv, bufs[i_fill], PORT_CAM1, CAM_BYTES, (s16)unit))) {
			ev_recv = 0;
			if (stop_requested(100000000LL))
				break;
			continue;
		}
		Handle hs[3] = {ev_stop, ev_recv, ev_err};
		s32 idx = -1;
		Result r = svcWaitSynchronizationN(&idx, hs, ev_err ? 3 : 2, false, STALL_NS);
		if (R_FAILED(r)) {
			if (stop_requested(100000000LL))
				break;
			continue;
		}
		if (r != 0) {
			// timed out: the sensor went quiet (lid closed, sleep). Switch it on again.
			svcCloseHandle(ev_recv);
			ev_recv = 0;
			CAMU_StopCapture(PORT_CAM1);
			CAMU_Activate(SELECT_NONE);
			sensor_start(sel);
			warm = 0;
			continue;
		}
		if (idx == 0)
			break;
		svcCloseHandle(ev_recv);
		ev_recv = 0;
		if (idx == 2) {
			// The camera's buffer overflowed and capture stopped. The frame is incomplete;
			// resume without switching the sensor off, so exposure and colours stay settled.
			CAMU_ClearBuffer(PORT_CAM1);
			CAMU_StartCapture(PORT_CAM1);
			// if it keeps overflowing, don't spin at high priority and freeze the app
			if (++overflows > 3 && stop_requested(30000000LL))
				break;
			continue;
		}
		overflows = 0;
		if (warm < WARMUP) {
			warm++;
			continue; // receive into the same buffer again
		}
		LightLock_Lock(&lock);
		int t = i_fill;
		i_fill = i_ready;
		i_ready = t;
		fresh = true;
		LightLock_Unlock(&lock);
	}

	if (ev_recv)
		svcCloseHandle(ev_recv);
	CAMU_StopCapture(PORT_CAM1);
	CAMU_ClearBuffer(PORT_CAM1);
	if (ev_err)
		svcCloseHandle(ev_err);
	CAMU_Activate(SELECT_NONE);
}

static bool thread_start(void)
{
	if (R_FAILED(svcCreateEvent(&ev_stop, RESET_ONESHOT)))
		return false;
	fresh = false;
	// above the main thread, so the next frame is requested the moment one completes; the
	// thread only runs for an instant per frame
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	thread = threadCreate(capture_thread, NULL, 16 * 1024, prio - 1 < 0x18 ? 0x18 : prio - 1, -2, false);
	if (!thread) {
		svcCloseHandle(ev_stop);
		return false;
	}
	thread_on = true;
	return true;
}

static void thread_stop(void)
{
	if (!thread_on)
		return;
	svcSignalEvent(ev_stop);
	threadJoin(thread, U64_MAX);
	threadFree(thread);
	svcCloseHandle(ev_stop);
	thread_on = false;
	fresh = false;
}

// ---------------------------------------------------------------- public

bool cam_open(bool use_front, char *err, size_t errlen)
{
	cam_close();
	if (!morton[63]) {
		morton_init();
		LightLock_Init(&lock);
	}
	for (int i = 0; i < 3; i++)
		bufs[i] = malloc(CAM_BYTES);
	tex_ok = C3D_TexInit(&tex, TEX_W, TEX_H, GPU_RGB565);
	if (!bufs[0] || !bufs[1] || !bufs[2] || !tex_ok) {
		snprintf(err, errlen, "Not enough memory for the camera");
		cam_close();
		return false;
	}
	C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
	memset(tex.data, 0, tex.size);
	C3D_TexFlush(&tex);

	Result r = camInit();
	if (R_FAILED(r)) {
		snprintf(err, errlen, "Camera unavailable (0x%08lX)", (unsigned long)r);
		cam_close();
		return false;
	}
	running = true;
	front = use_front;
	have_frame = frozen = false;
	i_fill = 0;
	i_ready = 1;
	i_show = 2;
	fps = fps_count = 0;
	fps_since = osGetTime();
	if (!thread_start()) {
		snprintf(err, errlen, "Could not start the camera");
		cam_close();
		return false;
	}
	return true;
}

void cam_close(void)
{
	if (running) {
		thread_stop(); // also switches the sensor off
		camExit();
		running = false;
	}
	if (tex_ok) {
		C3D_TexDelete(&tex);
		tex_ok = false;
	}
	for (int i = 0; i < 3; i++) {
		free(bufs[i]);
		bufs[i] = NULL;
	}
	have_frame = frozen = false;
}

bool cam_is_front(void) { return front; }
bool cam_has_frame(void) { return have_frame; }
bool cam_frozen(void) { return frozen; }
int  cam_fps(void) { return (int)fps; }

void cam_switch(void)
{
	if (!running || frozen)
		return;
	thread_stop();
	front = !front;
	have_frame = false;
	fps = fps_count = 0;
	fps_since = osGetTime();
	thread_start(); // if this fails the preview stays on "Starting camera..."; B leaves
}

// Call every frame: shows the newest finished camera frame.
void cam_update(void)
{
	if (!running)
		return;
	u64 now = osGetTime();
	if (now - fps_since >= 1000) {
		fps = fps_count * 1000 / (u32)(now - fps_since);
		fps_count = 0;
		fps_since = now;
	}
	if (frozen)
		return;
	bool got = false;
	LightLock_Lock(&lock);
	if (fresh) {
		int t = i_show;
		i_show = i_ready;
		i_ready = t;
		fresh = false;
		got = true;
	}
	LightLock_Unlock(&lock);
	if (got) {
		have_frame = true;
		fps_count++;
		upload_preview(bufs[i_show]);
	}
}

// Freezes the frame on screen; the camera keeps running so a retake is instant.
bool cam_snap(void)
{
	if (!running || !have_frame)
		return false;
	frozen = true;
	return true;
}

void cam_retake(void)
{
	frozen = false;
}

void cam_draw(float x, float y)
{
	if (!tex_ok || !have_frame)
		return;
	C2D_Image img = {&tex, &sub};
	C2D_DrawImageAt(img, x, y, 0.5f, NULL, 1.0f, 1.0f);
}

struct jerr {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
};

static void on_jpeg_error(j_common_ptr cinfo)
{
	longjmp(((struct jerr *)cinfo->err)->jump, 1);
}

// Encodes the snapshot as JPEG. *out is malloc'd by libjpeg; free() it.
bool cam_jpeg(unsigned char **out, unsigned long *outlen, int quality)
{
	if (!frozen || !bufs[i_show])
		return false;
	const u16 *snap = bufs[i_show];
	struct jpeg_compress_struct cinfo;
	struct jerr jerr;
	unsigned char *volatile row = malloc(CAM_W * 3);
	*out = NULL;
	*outlen = 0;
	if (!row)
		return false;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = on_jpeg_error;
	if (setjmp(jerr.jump)) {
		jpeg_destroy_compress(&cinfo);
		free(row);
		free(*out);
		*out = NULL;
		return false;
	}
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, out, outlen);
	cinfo.image_width = CAM_W;
	cinfo.image_height = CAM_H;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < CAM_H) {
		const u16 *src = snap + cinfo.next_scanline * CAM_W;
		for (u32 x = 0; x < CAM_W; x++) {
			u16 p = src[x];
			u8 r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
			row[x * 3 + 0] = (r << 3) | (r >> 2);
			row[x * 3 + 1] = (g << 2) | (g >> 4);
			row[x * 3 + 2] = (b << 3) | (b >> 2);
		}
		JSAMPROW rows[1] = {row};
		jpeg_write_scanlines(&cinfo, rows, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	free(row);
	return true;
}
