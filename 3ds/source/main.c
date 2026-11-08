// WA3DS: WhatsApp client for the Nintendo 3DS, talking to a wa3ds-bridge server.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PING_INTERVAL_MS   30000
#define RETRY_INTERVAL_MS  5000

static bool need_connect;   // connect on the next frame (so "Connecting..." is drawn first)
static int  connect_delay;  // frames to wait before connecting
static u64  retry_at;       // background reconnect time (0 = none)
static u64  last_ping;
static bool ever_authed;

static void fp_to_hex(const unsigned char fp[32], char out[65])
{
	static const char *hx = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		out[i * 2] = hx[fp[i] >> 4];
		out[i * 2 + 1] = hx[fp[i] & 15];
	}
	out[64] = 0;
}

static void start_connect(void)
{
	snprintf(g_app.status, sizeof g_app.status, "Connecting to %s:%d...", g_cfg.host, g_cfg.port);
	if (!ever_authed)
		g_app.screen = ST_CONNECTING;
	need_connect = true;
	connect_delay = 2;
}

static void begin_auth(void)
{
	client_reset();
	client_auth();
	last_ping = osGetTime();
	snprintf(g_app.status, sizeof g_app.status, "Logging in to the bridge...");
}

static void do_connect(void)
{
	need_connect = false;
	char err[128];
	unsigned char fp[32];
	if (!net_connect(g_cfg.host, g_cfg.port, fp, err, sizeof err)) {
		snprintf(g_app.status, sizeof g_app.status, "%s\n%s:%d\n\nRetrying in a few seconds.", err, g_cfg.host, g_cfg.port);
		if (!ever_authed)
			g_app.screen = ST_ERROR;
		retry_at = osGetTime() + RETRY_INTERVAL_MS;
		return;
	}
	retry_at = 0;

	char hex[65];
	fp_to_hex(fp, hex);
	if (g_cfg.pin[0] && strcmp(g_cfg.pin, hex) == 0) {
		begin_auth();
		return;
	}
	// unknown or changed certificate: ask the user
	memcpy(g_app.peer_fp, fp, 32);
	g_app.pin_mismatch = g_cfg.pin[0] != 0;
	g_app.screen = ST_TRUST;
}

static void run_setup(void)
{
	net_close();
	client_reset();
	if (config_prompt()) {
		ever_authed = false;
		start_connect();
	} else if (!g_cfg.host[0] || !g_cfg.token[0]) {
		snprintf(g_app.status, sizeof g_app.status, "Setup was cancelled.\nPress SELECT to set up the bridge connection.");
		g_app.screen = ST_ERROR;
	} else {
		start_connect();
	}
}

static void write_message(void)
{
	SwkbdState kb;
	static char buf[4096];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 1000);
	char hint[128];
	snprintf(hint, sizeof hint, "Message to %s", g_app.cur_name);
	swkbdSetHintText(&kb, hint);
	swkbdSetFeatures(&kb, SWKBD_MULTILINE | SWKBD_DARKEN_TOP_SCREEN);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Send", true);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM)
		return;
	size_t n = strlen(buf);
	while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\n'))
		buf[--n] = 0;
	if (n > 0)
		client_send_text(buf);
}

// ---------------------------------------------------------------- input

static touchPosition touch_start;
static float scroll_start;
static bool dragging, touching;

static void input_chats(u32 down, u32 repeat, u32 held)
{
	if (down & KEY_SELECT) {
		run_setup();
		return;
	}
	if (down & KEY_X)
		client_request_chats();

	if (repeat & KEY_DOWN && g_app.chat_sel < g_app.nchats - 1)
		g_app.chat_sel++;
	if (repeat & KEY_UP && g_app.chat_sel > 0)
		g_app.chat_sel--;
	if (repeat & KEY_RIGHT)
		g_app.chat_sel = g_app.chat_sel + 5 < g_app.nchats ? g_app.chat_sel + 5 : g_app.nchats - 1;
	if (repeat & KEY_LEFT)
		g_app.chat_sel = g_app.chat_sel - 5 > 0 ? g_app.chat_sel - 5 : 0;
	if (g_app.chat_sel < 0)
		g_app.chat_sel = 0;

	float list_h = SCR_H - 24;
	float max_scroll = g_app.nchats * 44.0f - list_h;
	if (max_scroll < 0)
		max_scroll = 0;

	if (repeat & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
		float y = g_app.chat_sel * 44.0f;
		if (y < g_app.chat_scroll)
			g_app.chat_scroll = y;
		if (y + 44 > g_app.chat_scroll + list_h)
			g_app.chat_scroll = y + 44 - list_h;
	}

	circlePosition cp;
	hidCircleRead(&cp);
	if (abs(cp.dy) > 20)
		g_app.chat_scroll -= cp.dy / 20.0f;

	// touch: drag to scroll, tap to open
	touchPosition tp;
	hidTouchRead(&tp);
	if (down & KEY_TOUCH) {
		touch_start = tp;
		scroll_start = g_app.chat_scroll;
		dragging = false;
		touching = true;
	} else if (held & KEY_TOUCH && touching) {
		int dy = tp.py - touch_start.py;
		if (abs(dy) > 8)
			dragging = true;
		if (dragging)
			g_app.chat_scroll = scroll_start - dy;
	} else if (touching && !(held & KEY_TOUCH)) {
		touching = false;
		if (!dragging) {
			int row = ui_chat_row_at(touch_start.py);
			if (row >= 0) {
				g_app.chat_sel = row;
				client_open_chat(&g_app.chats[row]);
				return;
			}
		}
	}

	if (g_app.chat_scroll > max_scroll)
		g_app.chat_scroll = max_scroll;
	if (g_app.chat_scroll < 0)
		g_app.chat_scroll = 0;

	if (down & KEY_A && g_app.chat_sel < g_app.nchats)
		client_open_chat(&g_app.chats[g_app.chat_sel]);
}

static void leave_chat(void)
{
	g_app.screen = ST_CHATS;
	msgs_clear();
	g_app.cur_jid[0] = 0;
}

static void chat_action(int action)
{
	switch (action) {
	case 0:
		write_message();
		break;
	case 1:
		if (g_app.msg_sel >= 0 && g_app.msg_sel < g_app.nmsgs && g_app.msgs[g_app.msg_sel].has_media)
			client_request_image(&g_app.msgs[g_app.msg_sel]);
		break;
	case 2:
		client_load_older();
		break;
	case 3:
		leave_chat();
		break;
	}
}

static void input_chat(u32 down, u32 repeat)
{
	if (repeat & KEY_UP) {
		if (g_app.msg_sel > 0) {
			g_app.msg_sel--;
			g_app.stick_bottom = false;
		} else {
			client_load_older();
		}
		ui_ensure_selected_visible();
	}
	if (repeat & KEY_DOWN && g_app.msg_sel < g_app.nmsgs - 1) {
		g_app.msg_sel++;
		g_app.stick_bottom = g_app.msg_sel == g_app.nmsgs - 1;
		ui_ensure_selected_visible();
	}

	circlePosition cp;
	hidCircleRead(&cp);
	if (abs(cp.dy) > 20) {
		g_app.view_top -= cp.dy / 15.0f;
		g_app.stick_bottom = false;
		float max_top = g_app.content_h - (SCR_H - HEADER_H);
		if (g_app.view_top > max_top)
			g_app.view_top = max_top;
		if (g_app.view_top < 0)
			g_app.view_top = 0;
	}

	if (down & KEY_A)
		chat_action(0);
	else if (down & KEY_Y)
		chat_action(1);
	else if (down & KEY_L)
		chat_action(2);
	else if (down & KEY_B)
		chat_action(3);
	else if (down & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		int b = ui_chat_button_at(tp.px, tp.py);
		if (b >= 0)
			chat_action(b);
	}
}

static void input_image(u32 down)
{
	if (down & (KEY_B | KEY_TOUCH | KEY_A)) {
		image_free();
		g_app.img_ready = false;
		g_app.screen = ST_CHAT;
	}
}

static void input_trust(u32 down)
{
	bool accept = g_app.pin_mismatch ? (down & KEY_SELECT) : (down & KEY_A);
	if (accept) {
		char hex[65];
		fp_to_hex(g_app.peer_fp, hex);
		strcpy(g_cfg.pin, hex);
		config_save();
		g_app.screen = ST_CONNECTING;
		begin_auth();
	} else if (down & KEY_B) {
		net_close();
		snprintf(g_app.status, sizeof g_app.status, "Certificate not trusted.\nPress SELECT to check the settings.");
		g_app.screen = ST_ERROR;
		retry_at = 0;
	}
}

// ---------------------------------------------------------------- main

int main(void)
{
	gfxInitDefault();
	hidSetRepeatParameters(20, 5);
	net_init();
	ui_init();
	g_app.stick_bottom = true;

	if (config_load()) {
		start_connect();
	} else {
		g_app.screen = ST_SETUP;
	}

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), repeat = hidKeysDownRepeat(), held = hidKeysHeld();
		if (down & KEY_START)
			break;

		switch (g_app.screen) {
		case ST_SETUP:
			ui_render(); // draw a frame behind the keyboard
			run_setup();
			continue;
		case ST_CONNECTING:
			if (down & KEY_SELECT) {
				run_setup();
				continue;
			}
			break;
		case ST_ERROR:
			if (down & KEY_SELECT) {
				run_setup();
				continue;
			}
			if (down & KEY_A || (retry_at && osGetTime() >= retry_at && g_cfg.host[0] && g_cfg.token[0] &&
			                     g_app.status[0] && !strstr(g_app.status, "rejected") && !strstr(g_app.status, "not trusted") &&
			                     !strstr(g_app.status, "cancelled")))
				start_connect();
			break;
		case ST_TRUST:
			input_trust(down);
			break;
		case ST_CHATS:
			input_chats(down, repeat, held);
			break;
		case ST_CHAT:
			input_chat(down, repeat);
			break;
		case ST_IMAGE:
			input_image(down);
			break;
		}

		// networking
		if (need_connect && --connect_delay <= 0)
			do_connect();

		if (net_connected()) {
			if (!net_poll(client_on_line, client_on_bin)) {
				// connection dropped
				client_reset();
				if (g_app.screen == ST_IMAGE && g_app.img_loading)
					strcpy(g_app.img_err, "Connection lost");
				if (ever_authed) {
					ui_show_toast("Connection to the bridge lost. Reconnecting...", 4);
					retry_at = osGetTime() + 2000;
				} else if (g_app.screen != ST_ERROR) {
					snprintf(g_app.status, sizeof g_app.status, "Connection closed by the bridge.\nRetrying in a few seconds.");
					g_app.screen = ST_ERROR;
					retry_at = osGetTime() + RETRY_INTERVAL_MS;
				}
			} else if (g_app.authed) {
				ever_authed = true;
				if (osGetTime() - last_ping > PING_INTERVAL_MS) {
					client_ping();
					last_ping = osGetTime();
				}
			}
		} else if (ever_authed && retry_at && osGetTime() >= retry_at && !need_connect) {
			start_connect(); // background reconnect, stays on the current screen
		}

		ui_render();
	}

	net_exit();
	ui_exit();
	gfxExit();
	return 0;
}
