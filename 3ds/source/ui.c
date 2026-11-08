// Rendering with citro2d: conversation on the top screen, chat list and controls on the bottom.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float g_line_h = 15.0f;

static C3D_RenderTarget *top, *bot;
static C2D_TextBuf tbuf;
static CFNT_s *font;

// palette
#define COL_BG        C2D_Color32(0xEC, 0xE5, 0xDD, 0xFF)
#define COL_WHITE     C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_HEADER    C2D_Color32(0x07, 0x5E, 0x54, 0xFF)
#define COL_ACCENT    C2D_Color32(0x25, 0xD3, 0x66, 0xFF)
#define COL_OWN       C2D_Color32(0xDC, 0xF8, 0xC6, 0xFF)
#define COL_TEXT      C2D_Color32(0x11, 0x1B, 0x21, 0xFF)
#define COL_MUTED     C2D_Color32(0x66, 0x77, 0x81, 0xFF)
#define COL_SENDER    C2D_Color32(0x12, 0x8C, 0x7E, 0xFF)
#define COL_SEL       C2D_Color32(0x34, 0xB7, 0xF1, 0xFF)
#define COL_ROW_SEL   C2D_Color32(0xE9, 0xED, 0xEF, 0xFF)
#define COL_LINE      C2D_Color32(0xE0, 0xE0, 0xE0, 0xFF)
#define COL_TOAST     C2D_Color32(0x20, 0x2C, 0x33, 0xF0)
#define COL_WARN      C2D_Color32(0xC0, 0x39, 0x2B, 0xFF)
#define COL_BTN       C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_BTN_OFF   C2D_Color32(0xF2, 0xF2, 0xF2, 0xFF)
#define COL_BLACK     C2D_Color32(0x00, 0x00, 0x00, 0xFF)

#define BOT_HEADER_H 24
#define ROW_H        44

void ui_init(void)
{
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
	C2D_Prepare();
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tbuf = C2D_TextBufNew(16384);
	fontEnsureMapped();
	font = fontGetSystemFont();
	FINF_s *fi = fontGetInfo(font);
	if (fi && fi->lineFeed)
		g_line_h = fi->lineFeed * MSG_SCALE;
}

void ui_exit(void)
{
	image_free();
	C2D_TextBufDelete(tbuf);
	C2D_Fini();
	C3D_Fini();
}

// ---------------------------------------------------------------- text helpers

static float glyph_adv(u32 cp, float scale)
{
	fontGlyphPos_s pos;
	fontCalcGlyphPos(&pos, font, fontGlyphIndexFromCodePoint(font, cp), 0, scale, scale);
	return pos.xAdvance;
}

static int next_cp(const char *s, u32 *cp)
{
	ssize_t n = decode_utf8(cp, (const uint8_t *)s);
	if (n <= 0) {
		*cp = '?';
		return 1;
	}
	return (int)n;
}

float ui_text_width(const char *s, float scale)
{
	float w = 0, best = 0;
	while (*s) {
		u32 cp;
		s += next_cp(s, &cp);
		if (cp == '\n') {
			if (w > best)
				best = w;
			w = 0;
			continue;
		}
		w += glyph_adv(cp, scale);
	}
	return w > best ? w : best;
}

char *ui_wrap(const char *text, float maxw, float scale, int *lines, float *widest)
{
	size_t n = strlen(text);
	char *out = malloc(n * 2 + 2);
	size_t o = 0;
	float w = 0, since_space = 0, best = 0, w_before_space = 0;
	long space_at = -1;
	int nl = 1;
	const char *p = text;

	while (*p) {
		u32 cp;
		int len = next_cp(p, &cp);
		if (cp == '\r') {
			p += len;
			continue;
		}
		if (cp == '\n') {
			out[o++] = '\n';
			if (w > best)
				best = w;
			w = since_space = 0;
			space_at = -1;
			nl++;
			p += len;
			continue;
		}
		float a = glyph_adv(cp, scale);
		if (w + a > maxw && w > 0 && cp != ' ') {
			if (space_at >= 0) {
				// break at the last space on this line
				out[space_at] = '\n';
				if (w_before_space > best)
					best = w_before_space;
				w = since_space;
			} else {
				out[o++] = '\n';
				if (w > best)
					best = w;
				w = 0;
			}
			space_at = -1;
			since_space = 0;
			nl++;
			if (w + a > maxw && w > 0) { // the carried-over word is itself too long
				out[o++] = '\n';
				if (w > best)
					best = w;
				w = 0;
				nl++;
			}
		}
		if (cp == ' ') {
			space_at = (long)o;
			w_before_space = w;
			since_space = 0;
		} else {
			since_space += a;
		}
		memcpy(out + o, p, len);
		o += len;
		w += a;
		p += len;
	}
	out[o] = 0;
	if (w > best)
		best = w;
	if (lines)
		*lines = nl;
	if (widest)
		*widest = best;
	return out;
}

// Copies s into out, cutting it with "…" so it fits in maxw.
static void ellipsize(const char *s, float maxw, float scale, char *out, size_t n)
{
	float w = 0, ell = glyph_adv(0x2026, scale);
	size_t o = 0;
	const char *p = s;
	while (*p) {
		u32 cp;
		int len = next_cp(p, &cp);
		if (cp == '\n')
			cp = ' ';
		float a = glyph_adv(cp, scale);
		if (w + a > maxw) {
			// back off until the ellipsis fits
			while (o > 0 && w + ell > maxw) {
				do
					o--;
				while (o > 0 && ((unsigned char)out[o] & 0xC0) == 0x80);
				u32 c2;
				next_cp(out + o, &c2);
				w -= glyph_adv(c2, scale);
			}
			if (o + 4 < n) {
				memcpy(out + o, "\xE2\x80\xA6", 3);
				o += 3;
			}
			break;
		}
		if (o + len + 4 >= n)
			break;
		if (*p == '\n')
			out[o] = ' ';
		else
			memcpy(out + o, p, len);
		o += len;
		w += a;
		p += len;
	}
	out[o] = 0;
}

static void text(const char *s, float x, float y, float scale, u32 color, u32 flags)
{
	C2D_Text t;
	C2D_TextParse(&t, tbuf, s);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

static void text_fit(const char *s, float x, float y, float maxw, float scale, u32 color)
{
	char buf[256];
	ellipsize(s, maxw, scale, buf, sizeof buf);
	text(buf, x, y, scale, color, 0);
}

static void rect(float x, float y, float w, float h, u32 c)
{
	C2D_DrawRectSolid(x, y, 0.5f, w, h, c);
}

static void header(const char *title, float w, float h, const char *right)
{
	rect(0, 0, w, h, COL_HEADER);
	text_fit(title, 6, (h - 16) / 2, w - 90, 0.55f, COL_WHITE);
	if (right)
		text(right, w - 6, (h - 13) / 2, 0.45f, COL_WHITE, C2D_AlignRight);
}

void ui_show_toast(const char *s, int seconds)
{
	strlcpy_utf8(g_app.toast, s, sizeof g_app.toast);
	g_app.toast_until = osGetTime() + (u64)seconds * 1000;
}

static void draw_toast(void)
{
	if (!g_app.toast[0] || osGetTime() > g_app.toast_until)
		return;
	int lines;
	float widest;
	char *w = ui_wrap(g_app.toast, TOP_W - 40, 0.45f, &lines, &widest);
	if (lines > 3)
		lines = 3;
	float h = lines * 14 + 12;
	rect(10, SCR_H - h - 8, TOP_W - 20, h, COL_TOAST);
	text(w, 20, SCR_H - h - 2, 0.45f, COL_WHITE, 0);
	free(w);
}

static const char *status_label(void)
{
	if (!net_connected())
		return "Offline";
	if (!g_app.wa_online)
		return "Bridge OK, WhatsApp offline";
	return "Online";
}

// ---------------------------------------------------------------- top screen

void ui_ensure_selected_visible(void)
{
	float vh = SCR_H - HEADER_H;
	if (g_app.nmsgs > 0 && g_app.msg_sel >= 0 && g_app.msg_sel < g_app.nmsgs) {
		Msg *m = &g_app.msgs[g_app.msg_sel];
		if (m->y - 4 < g_app.view_top)
			g_app.view_top = m->y - 4;
		if (m->y + m->h + 4 > g_app.view_top + vh)
			g_app.view_top = m->y + m->h + 4 - vh;
	}
	float max_top = g_app.content_h - vh;
	if (g_app.view_top > max_top)
		g_app.view_top = max_top;
	if (g_app.view_top < 0)
		g_app.view_top = 0;
}

static void draw_conversation(void)
{
	C2D_TargetClear(top, COL_BG);
	C2D_SceneBegin(top);
	C2D_TextBufClear(tbuf);

	float vh = SCR_H - HEADER_H;
	for (int i = 0; i < g_app.nmsgs; i++) {
		Msg *m = &g_app.msgs[i];
		float y = HEADER_H + m->y - g_app.view_top;
		if (y + m->h < HEADER_H || y > SCR_H)
			continue;

		char tbuf_time[24];
		fmt_time(m->ts, tbuf_time, sizeof tbuf_time, false);
		bool show_sender = g_app.cur_group && !m->from_me;
		float bw = m->text_w;
		float tw = ui_text_width(tbuf_time, 0.4f);
		if (tw > bw)
			bw = tw;
		if (show_sender) {
			float sw = ui_text_width(m->sender, 0.4f);
			if (sw > bw)
				bw = sw < BUBBLE_MAX_W - 12 ? sw : BUBBLE_MAX_W - 12;
		}
		bw += 12;
		if (bw < 48)
			bw = 48;
		float x = m->from_me ? TOP_W - 8 - bw : 8;

		if (i == g_app.msg_sel)
			rect(x - 2, y - 2, bw + 4, m->h + 4, COL_SEL);
		rect(x, y, bw, m->h, m->from_me ? COL_OWN : COL_WHITE);

		float ty = y + 4;
		if (show_sender) {
			text_fit(m->sender[0] ? m->sender : "?", x + 6, ty, bw - 12, 0.4f, COL_SENDER);
			ty += 12;
		}
		bool deleted = !strcmp(m->kind, "deleted");
		text(m->wrapped, x + 6, ty, MSG_SCALE, deleted ? COL_MUTED : COL_TEXT, 0);
		text(tbuf_time, x + bw - 6, y + m->h - 14, 0.4f, COL_MUTED, C2D_AlignRight);
		if (m->has_media)
			text("Y: view", x + 6, y + m->h - 14, 0.4f, COL_SENDER, 0);
	}

	if (g_app.nmsgs == 0)
		text(g_app.loading_msgs ? "Loading..." : "No messages yet", TOP_W / 2, 110, 0.5f, COL_MUTED, C2D_AlignCenter);
	else if (g_app.view_top <= 0 && g_app.loading_msgs)
		text("Loading older messages...", TOP_W / 2, HEADER_H + 2, 0.4f, COL_MUTED, C2D_AlignCenter);
	(void)vh;

	header(g_app.cur_name, TOP_W, HEADER_H, status_label());
	draw_toast();
}

static void draw_top_chats(void)
{
	C2D_TargetClear(top, COL_BG);
	C2D_SceneBegin(top);
	C2D_TextBufClear(tbuf);
	header("WA3DS", TOP_W, HEADER_H, status_label());

	if (g_app.nchats == 0) {
		text("No chats yet.", TOP_W / 2, 90, 0.6f, COL_TEXT, C2D_AlignCenter);
		text("New messages will appear here. Older chats arrive\nshortly after you first link the bridge.",
		     TOP_W / 2, 120, 0.45f, COL_MUTED, C2D_AlignCenter);
	} else if (g_app.chat_sel >= 0 && g_app.chat_sel < g_app.nchats) {
		Chat *c = &g_app.chats[g_app.chat_sel];
		rect(10, 30, TOP_W - 20, 150, COL_WHITE);
		text_fit(c->name, 20, 38, TOP_W - 40, 0.7f, COL_TEXT);
		char t[32];
		fmt_time(c->ts, t, sizeof t, true);
		text(t, 20, 64, 0.42f, COL_MUTED, 0);
		if (c->unread > 0) {
			char u[32];
			snprintf(u, sizeof u, "%d unread", c->unread);
			text(u, TOP_W - 20, 64, 0.42f, COL_SENDER, C2D_AlignRight);
		}
		int lines;
		char *w = ui_wrap(c->preview, TOP_W - 40, 0.5f, &lines, NULL);
		text(w, 20, 86, 0.5f, COL_TEXT, 0);
		free(w);
	}
	text("A: open   X: refresh   SELECT: settings   START: quit", TOP_W / 2, 196, 0.4f, COL_MUTED, C2D_AlignCenter);
	if (g_app.own_name[0]) {
		char me[96];
		snprintf(me, sizeof me, "Linked as %s", g_app.own_name);
		text(me, TOP_W / 2, 212, 0.4f, COL_MUTED, C2D_AlignCenter);
	}
	draw_toast();
}

static void draw_top_image(void)
{
	C2D_TargetClear(top, COL_BLACK);
	C2D_SceneBegin(top);
	C2D_TextBufClear(tbuf);
	if (g_app.img_ready) {
		image_draw(0, 0, TOP_W, SCR_H);
	} else if (g_app.img_err[0]) {
		text(g_app.img_err, TOP_W / 2, 110, 0.5f, COL_WHITE, C2D_AlignCenter);
	} else {
		size_t total, got = net_bin_progress(&total);
		char t[64];
		if (total)
			snprintf(t, sizeof t, "Loading image... %u%%", (unsigned)(got * 100 / total));
		else
			snprintf(t, sizeof t, "Loading image...");
		text(t, TOP_W / 2, 110, 0.5f, COL_WHITE, C2D_AlignCenter);
	}
}

static void hex_fp(const unsigned char fp[32], char *out)
{
	// 16 groups of 4 hex digits, 4 groups per line
	static const char *hx = "0123456789ABCDEF";
	char *o = out;
	for (int i = 0; i < 32; i++) {
		*o++ = hx[fp[i] >> 4];
		*o++ = hx[fp[i] & 15];
		if (i % 2 == 1 && i != 31)
			*o++ = (i % 8 == 7) ? '\n' : ' ';
	}
	*o = 0;
}

static void draw_top_message_screen(void)
{
	C2D_TargetClear(top, COL_BG);
	C2D_SceneBegin(top);
	C2D_TextBufClear(tbuf);
	header("WA3DS " APP_VERSION, TOP_W, HEADER_H, NULL);

	if (g_app.screen == ST_TRUST) {
		char fp[96];
		hex_fp(g_app.peer_fp, fp);
		if (g_app.pin_mismatch) {
			text("The bridge certificate has CHANGED!", TOP_W / 2, 30, 0.6f, COL_WARN, C2D_AlignCenter);
			text("This can mean someone is intercepting the connection.\nOnly continue if you reset or moved the bridge.",
			     TOP_W / 2, 54, 0.42f, COL_TEXT, C2D_AlignCenter);
		} else {
			text("First connection to this bridge", TOP_W / 2, 30, 0.6f, COL_TEXT, C2D_AlignCenter);
			text("Check that this fingerprint matches the one\nprinted by the bridge on your server:",
			     TOP_W / 2, 54, 0.45f, COL_TEXT, C2D_AlignCenter);
		}
		rect(60, 92, TOP_W - 120, 76, COL_WHITE);
		text(fp, TOP_W / 2, 98, 0.55f, COL_TEXT, C2D_AlignCenter);
		text(g_app.pin_mismatch ? "SELECT: trust new certificate     B: cancel" : "A: trust     B: cancel",
		     TOP_W / 2, 190, 0.5f, COL_HEADER, C2D_AlignCenter);
		return;
	}

	const char *title = g_app.screen == ST_ERROR ? "Problem" : "Connecting";
	text(title, TOP_W / 2, 70, 0.7f, g_app.screen == ST_ERROR ? COL_WARN : COL_TEXT, C2D_AlignCenter);
	text(g_app.status, TOP_W / 2, 105, 0.48f, COL_TEXT, C2D_AlignCenter);
	draw_toast();
}

// ---------------------------------------------------------------- bottom screen

int ui_chat_row_at(int y)
{
	if (y < BOT_HEADER_H)
		return -1;
	int i = (int)((y - BOT_HEADER_H + g_app.chat_scroll) / ROW_H);
	return i >= 0 && i < g_app.nchats ? i : -1;
}

static void draw_bot_chats(void)
{
	C2D_TargetClear(bot, COL_WHITE);
	C2D_SceneBegin(bot);
	C2D_TextBufClear(tbuf);

	for (int i = 0; i < g_app.nchats; i++) {
		float y = BOT_HEADER_H + i * ROW_H - g_app.chat_scroll;
		if (y + ROW_H < BOT_HEADER_H || y > SCR_H)
			continue;
		Chat *c = &g_app.chats[i];
		if (i == g_app.chat_sel)
			rect(0, y, BOT_W, ROW_H, COL_ROW_SEL);
		char t[24];
		fmt_time(c->ts, t, sizeof t, false);
		float tw = ui_text_width(t, 0.4f);
		text_fit(c->name, 8, y + 4, BOT_W - 24 - tw, 0.55f, COL_TEXT);
		text(t, BOT_W - 8, y + 6, 0.4f, c->unread ? COL_ACCENT : COL_MUTED, C2D_AlignRight);
		text_fit(c->preview, 8, y + 24, BOT_W - (c->unread ? 44 : 16), 0.45f, COL_MUTED);
		if (c->unread > 0) {
			char u[8];
			snprintf(u, sizeof u, "%d", c->unread > 99 ? 99 : c->unread);
			C2D_DrawCircleSolid(BOT_W - 18, y + 31, 0.5f, 9, COL_ACCENT);
			text(u, BOT_W - 18, y + 25, 0.4f, COL_WHITE, C2D_AlignCenter);
		}
		rect(8, y + ROW_H - 1, BOT_W - 16, 1, COL_LINE);
	}

	char title[32];
	snprintf(title, sizeof title, "Chats (%d)", g_app.nchats);
	header(title, BOT_W, BOT_HEADER_H, NULL);
}

static const char *btn_labels[5] = {"Write (A)", "Image (Y)", "Call (X)", "Older (L)", "Back (B)"};

// top row: write, image, call; bottom row: older, back
static void btn_rect(int i, float *x, float *y, float *w, float *h)
{
	*h = 60;
	if (i < 3) {
		*w = 98;
		*x = 8 + i * 103;
		*y = 104;
	} else {
		*w = 150;
		*x = 8 + (i - 3) * 154;
		*y = 170;
	}
}

int ui_chat_button_at(int x, int y)
{
	for (int i = 0; i < 5; i++) {
		float bx, by, bw, bh;
		btn_rect(i, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh)
			return i;
	}
	return -1;
}

static bool selected_has_media(void)
{
	return g_app.msg_sel >= 0 && g_app.msg_sel < g_app.nmsgs && g_app.msgs[g_app.msg_sel].has_media;
}

static void draw_bot_chat(void)
{
	C2D_TargetClear(bot, COL_BG);
	C2D_SceneBegin(bot);
	C2D_TextBufClear(tbuf);
	header(g_app.cur_name, BOT_W, BOT_HEADER_H, NULL);

	if (g_app.msg_sel >= 0 && g_app.msg_sel < g_app.nmsgs) {
		Msg *m = &g_app.msgs[g_app.msg_sel];
		char t[32], who[96];
		fmt_time(m->ts, t, sizeof t, true);
		snprintf(who, sizeof who, "%s", m->from_me ? "You" : (m->sender[0] ? m->sender : g_app.cur_name));
		text_fit(who, 10, 34, BOT_W - 20, 0.5f, COL_SENDER);
		text(t, 10, 54, 0.42f, COL_MUTED, 0);
		char pos[32];
		snprintf(pos, sizeof pos, "%d / %d", g_app.msg_sel + 1, g_app.nmsgs);
		text(pos, BOT_W - 10, 54, 0.42f, COL_MUTED, C2D_AlignRight);
	}
	text("D-Pad: select   Circle Pad: scroll", BOT_W / 2, 78, 0.4f, COL_MUTED, C2D_AlignCenter);

	for (int i = 0; i < 5; i++) {
		float x, y, w, h;
		btn_rect(i, &x, &y, &w, &h);
		bool enabled = true;
		if (i == 1)
			enabled = selected_has_media();
		if (i == 2)
			enabled = !g_app.cur_group;
		if (i == 3 && g_app.no_older)
			enabled = false;
		rect(x, y, w, h, enabled ? COL_BTN : COL_BTN_OFF);
		rect(x, y + h - 3, w, 3, enabled ? COL_ACCENT : COL_LINE);
		text(btn_labels[i], x + w / 2, y + h / 2 - 9, 0.5f, enabled ? COL_TEXT : COL_MUTED, C2D_AlignCenter);
	}
}

static void draw_bot_image(void)
{
	C2D_TargetClear(bot, COL_BLACK);
	C2D_SceneBegin(bot);
	C2D_TextBufClear(tbuf);
	text("Tap or press B to go back", BOT_W / 2, 110, 0.5f, COL_WHITE, C2D_AlignCenter);
}

static void draw_bot_hints(void)
{
	C2D_TargetClear(bot, COL_WHITE);
	C2D_SceneBegin(bot);
	C2D_TextBufClear(tbuf);
	const char *h = "START: quit";
	if (g_app.screen == ST_ERROR)
		h = "A: retry now\nSELECT: change settings\nSTART: quit";
	else if (g_app.screen == ST_CONNECTING)
		h = "SELECT: change settings\nSTART: quit";
	text(h, BOT_W / 2, 90, 0.5f, COL_TEXT, C2D_AlignCenter);
}

// ---------------------------------------------------------------- call screen

#define COL_CALL_BG  C2D_Color32(0x0B, 0x14, 0x1A, 0xFF)
#define COL_RED      C2D_Color32(0xE5, 0x39, 0x35, 0xFF)

static void call_status(char *out, size_t n)
{
	switch (g_app.call_state) {
	case CALL_RINGING:
		snprintf(out, n, "Calling...");
		break;
	case CALL_INCOMING:
		snprintf(out, n, "Incoming voice call");
		break;
	case CALL_CONNECTING:
		snprintf(out, n, "Connecting...");
		break;
	case CALL_ACTIVE: {
		u64 s = (osGetTime() - g_app.call_started) / 1000;
		snprintf(out, n, "%02u:%02u%s", (unsigned)(s / 60), (unsigned)(s % 60), g_app.call_muted ? "  (muted)" : "");
		break;
	}
	default: {
		const char *r = g_app.call_end_reason;
		if (!strcmp(r, "declined") || !strcmp(r, "busy"))
			snprintf(out, n, "Call declined");
		else if (!strcmp(r, "timeout"))
			snprintf(out, n, "No answer");
		else if (r[0] && strcmp(r, "user_ended") && strcmp(r, "unknown"))
			snprintf(out, n, "Call ended (%s)", r);
		else
			snprintf(out, n, "Call ended");
	}
	}
}

static void draw_top_call(void)
{
	C2D_TargetClear(top, COL_CALL_BG);
	C2D_SceneBegin(top);
	C2D_TextBufClear(tbuf);

	// avatar circle with initial
	C2D_DrawCircleSolid(TOP_W / 2, 70, 0.5f, 34, COL_HEADER);
	char initial[8] = "?";
	if (g_app.call_name[0]) {
		u32 cp;
		int len = next_cp(g_app.call_name, &cp);
		memcpy(initial, g_app.call_name, len);
		initial[len] = 0;
	}
	text(initial, TOP_W / 2, 52, 1.0f, COL_WHITE, C2D_AlignCenter);

	char name[128];
	ellipsize(g_app.call_name, TOP_W - 40, 0.75f, name, sizeof name);
	text(name, TOP_W / 2, 116, 0.75f, COL_WHITE, C2D_AlignCenter);
	char st[64];
	call_status(st, sizeof st);
	text(st, TOP_W / 2, 146, 0.55f, C2D_Color32(0xB0, 0xBE, 0xC5, 0xFF), C2D_AlignCenter);

	if (g_app.call_state == CALL_ACTIVE) {
		// microphone level meter
		float l = audio_mic_level();
		if (l > 1)
			l = 1;
		rect(120, 180, 160, 6, C2D_Color32(0x26, 0x32, 0x38, 0xFF));
		rect(120, 180, 160 * (g_app.call_muted ? 0 : l), 6, COL_ACCENT);
		text(g_app.call_muted ? "Microphone muted" : "Microphone", TOP_W / 2, 190, 0.4f, COL_MUTED, C2D_AlignCenter);
	}
	if (!audio_available())
		text("No sound: dump the DSP firmware with DSP1", TOP_W / 2, 212, 0.42f, COL_RED, C2D_AlignCenter);
	else if (g_app.call_state == CALL_ACTIVE || g_app.call_state == CALL_CONNECTING)
		text("Tip: use headphones to avoid echo", TOP_W / 2, 214, 0.4f, COL_MUTED, C2D_AlignCenter);
	draw_toast();
}

static void call_btn_rect(int i, float *x, float *y, float *w, float *h)
{
	*x = i ? 166 : 10;
	*y = 110;
	*w = 144;
	*h = 110;
}

int ui_call_button_at(int x, int y)
{
	for (int i = 0; i < 2; i++) {
		float bx, by, bw, bh;
		call_btn_rect(i, &bx, &by, &bw, &bh);
		if (x >= bx && x < bx + bw && y >= by && y < by + bh)
			return i;
	}
	return -1;
}

static void draw_bot_call(void)
{
	C2D_TargetClear(bot, COL_CALL_BG);
	C2D_SceneBegin(bot);
	C2D_TextBufClear(tbuf);

	const char *l = NULL, *r = NULL;
	u32 lc = C2D_Color32(0x37, 0x47, 0x4F, 0xFF), rc = COL_RED;
	switch (g_app.call_state) {
	case CALL_INCOMING:
		l = "Decline (B)";
		lc = COL_RED;
		r = "Accept (A)";
		rc = COL_ACCENT;
		break;
	case CALL_RINGING:
	case CALL_CONNECTING:
		r = "Hang up (B)";
		break;
	case CALL_ACTIVE:
		l = g_app.call_muted ? "Unmute (X)" : "Mute (X)";
		r = "Hang up (B)";
		break;
	default:
		break;
	}
	text("Voice call", BOT_W / 2, 40, 0.55f, COL_WHITE, C2D_AlignCenter);
	for (int i = 0; i < 2; i++) {
		const char *label = i ? r : l;
		if (!label)
			continue;
		float x, y, w, h;
		call_btn_rect(i, &x, &y, &w, &h);
		rect(x, y, w, h, i ? rc : lc);
		text(label, x + w / 2, y + h / 2 - 10, 0.6f, COL_WHITE, C2D_AlignCenter);
	}
}

void ui_render(void)
{
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	switch (g_app.screen) {
	case ST_CHATS:
		draw_top_chats();
		draw_bot_chats();
		break;
	case ST_CHAT:
		draw_conversation();
		draw_bot_chat();
		break;
	case ST_IMAGE:
		draw_top_image();
		draw_bot_image();
		break;
	case ST_CALL:
		draw_top_call();
		draw_bot_call();
		break;
	default:
		draw_top_message_screen();
		draw_bot_hints();
		break;
	}
	C3D_FrameEnd(0);
}
