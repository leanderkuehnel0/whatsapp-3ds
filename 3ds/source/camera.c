// Camera capture for sending photos: live VGA preview, snapshot, JPEG encoding.
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
#define STALL_MS  2000 // restart capture if no frame arrives for this long

static bool   running, receiving, have_frame, frozen, front;
static u16   *frame, *snap;
static Handle ev;
static u32    xfer;
static u64    last_frame_at;
static C3D_Tex tex;
static bool   tex_ok;
static const Tex3DS_SubTexture sub = {PREV_W, PREV_H, 0.0f, 1.0f, (float)PREV_W / TEX_W, 1.0f - (float)PREV_H / TEX_H};

static inline u32 tile_offset(u32 x, u32 y)
{
	return (((y >> 3) * (TEX_W >> 3) + (x >> 3)) << 6) +
	       ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

// Downscales a VGA RGB565 frame by 2 into the preview texture.
static void upload_preview(const u16 *src)
{
	if (!tex_ok)
		return;
	u16 *dst = (u16 *)tex.data;
	for (u32 y = 0; y < PREV_H; y++) {
		const u16 *row = src + (y * 2) * CAM_W;
		for (u32 x = 0; x < PREV_W; x++)
			dst[tile_offset(x, y)] = row[x * 2];
	}
	C3D_TexFlush(&tex);
}

static void capture_stop(void)
{
	if (receiving) {
		svcCloseHandle(ev);
		receiving = false;
	}
	CAMU_StopCapture(PORT_CAM1);
}

static void capture_start(void)
{
	// the inner camera and the outer right camera both deliver on port 1
	CAMU_Activate(front ? SELECT_IN1 : SELECT_OUT1);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);
	last_frame_at = osGetTime();
}

bool cam_open(bool use_front, char *err, size_t errlen)
{
	cam_close();
	frame = malloc(CAM_BYTES);
	snap = malloc(CAM_BYTES);
	tex_ok = C3D_TexInit(&tex, TEX_W, TEX_H, GPU_RGB565);
	if (!frame || !snap || !tex_ok) {
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
	CAMU_SetSize(SELECT_ALL, SIZE_VGA, CONTEXT_A);
	CAMU_SetOutputFormat(SELECT_ALL, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(SELECT_ALL, FRAME_RATE_15);
	CAMU_SetNoiseFilter(SELECT_ALL, true);
	CAMU_SetAutoExposure(SELECT_ALL, true);
	CAMU_SetAutoWhiteBalance(SELECT_ALL, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	CAMU_GetMaxBytes(&xfer, CAM_W, CAM_H);
	CAMU_SetTransferBytes(PORT_CAM1, xfer, CAM_W, CAM_H);

	front = use_front;
	running = true;
	have_frame = frozen = false;
	capture_start();
	return true;
}

void cam_close(void)
{
	if (running) {
		capture_stop();
		CAMU_Activate(SELECT_NONE);
		camExit();
		running = false;
	}
	if (tex_ok) {
		C3D_TexDelete(&tex);
		tex_ok = false;
	}
	free(frame);
	free(snap);
	frame = snap = NULL;
	have_frame = frozen = false;
}

bool cam_is_front(void) { return front; }
bool cam_has_frame(void) { return have_frame; }
bool cam_frozen(void) { return frozen; }

void cam_switch(void)
{
	if (!running || frozen)
		return;
	capture_stop();
	CAMU_Activate(SELECT_NONE);
	front = !front;
	have_frame = false;
	capture_start();
}

// Call every frame: picks up a finished camera frame and shows it.
void cam_update(void)
{
	if (!running || frozen)
		return;
	if (!receiving) {
		CAMU_SetReceiving(&ev, frame, PORT_CAM1, CAM_BYTES, (s16)xfer);
		receiving = true;
	}
	if (svcWaitSynchronization(ev, 0) == 0) { // 0 = signalled (a timeout is a different code)
		svcCloseHandle(ev);
		receiving = false;
		have_frame = true;
		last_frame_at = osGetTime();
		upload_preview(frame);
	} else if (osGetTime() - last_frame_at > STALL_MS) {
		capture_stop(); // e.g. after the lid was closed: start over
		capture_start();
	}
}

bool cam_snap(void)
{
	if (!running || !have_frame)
		return false;
	memcpy(snap, frame, CAM_BYTES);
	frozen = true;
	upload_preview(snap);
	return true;
}

void cam_retake(void)
{
	frozen = false;
	if (running)
		CAMU_ClearBuffer(PORT_CAM1);
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
	if (!frozen || !snap)
		return false;
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
