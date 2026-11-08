// Decodes a JPEG from the bridge into a GPU texture for the image viewer.
#include "app.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define TEX_W 512
#define TEX_H 256

static C3D_Tex tex;
static Tex3DS_SubTexture sub;
static bool have_tex;
static int img_w, img_h;

struct jerr {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
	char msg[JMSG_LENGTH_MAX];
};

static void on_jpeg_error(j_common_ptr cinfo)
{
	struct jerr *e = (struct jerr *)cinfo->err;
	(*cinfo->err->format_message)(cinfo, e->msg);
	longjmp(e->jump, 1);
}

// Offset of pixel (x, y) in the GPU's 8×8 Morton-tiled texture layout.
static inline u32 tile_offset(u32 x, u32 y)
{
	return (((y >> 3) * (TEX_W >> 3) + (x >> 3)) << 6) +
	       ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

void image_free(void)
{
	if (have_tex) {
		C3D_TexDelete(&tex);
		have_tex = false;
	}
}

bool image_set_jpeg(const unsigned char *data, size_t len, char *err, size_t errlen)
{
	image_free();

	struct jpeg_decompress_struct cinfo;
	struct jerr jerr;
	unsigned char *volatile row = NULL;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = on_jpeg_error;
	if (setjmp(jerr.jump)) {
		snprintf(err, errlen, "JPEG error: %s", jerr.msg);
		jpeg_destroy_decompress(&cinfo);
		free(row);
		image_free();
		return false;
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (unsigned char *)data, len);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	// The bridge already sizes images for the screen; shrink anything larger as a fallback.
	while ((cinfo.image_width / cinfo.scale_denom > TEX_W || cinfo.image_height / cinfo.scale_denom > TEX_H) &&
	       cinfo.scale_denom < 8)
		cinfo.scale_denom *= 2;
	jpeg_start_decompress(&cinfo);

	img_w = cinfo.output_width < TEX_W ? cinfo.output_width : TEX_W;
	img_h = cinfo.output_height < TEX_H ? cinfo.output_height : TEX_H;

	if (!C3D_TexInit(&tex, TEX_W, TEX_H, GPU_RGBA8)) {
		snprintf(err, errlen, "Out of video memory");
		jpeg_destroy_decompress(&cinfo);
		return false;
	}
	have_tex = true;
	C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
	memset(tex.data, 0, tex.size);

	u32 *px = (u32 *)tex.data;
	row = malloc(cinfo.output_width * 3);
	while (cinfo.output_scanline < cinfo.output_height) {
		u32 y = cinfo.output_scanline;
		unsigned char *rows[1] = {row};
		jpeg_read_scanlines(&cinfo, rows, 1);
		if (y >= TEX_H)
			continue;
		for (u32 x = 0; x < (u32)img_w; x++) {
			unsigned char *p = row + x * 3;
			px[tile_offset(x, y)] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | 0xFF;
		}
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(row);

	C3D_TexFlush(&tex);
	sub.width = img_w;
	sub.height = img_h;
	sub.left = 0.0f;
	sub.top = 1.0f;
	sub.right = (float)img_w / TEX_W;
	sub.bottom = 1.0f - (float)img_h / TEX_H;
	return true;
}

void image_draw(float x, float y, float maxw, float maxh)
{
	if (!have_tex)
		return;
	float s = maxw / img_w;
	if (maxh / img_h < s)
		s = maxh / img_h;
	if (s > 1.0f)
		s = 1.0f;
	float w = img_w * s, h = img_h * s;
	C2D_Image img = {&tex, &sub};
	C2D_DrawImageAt(img, x + (maxw - w) / 2, y + (maxh - h) / 2, 0.5f, NULL, s, s);
}
