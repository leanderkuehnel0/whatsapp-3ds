// WA3DS: WhatsApp client for the Nintendo 3DS, talking to a wa3ds-bridge server.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PING_INTERVAL_MS   30000
#define RETRY_INTERVAL_MS  5000

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
	retry_at = 0;
	if (!net_connect_begin(g_cfg.host, g_cfg.port)) {
		snprintf(g_app.status, sizeof g_app.status, "Could not start connecting.\nRetrying in a few seconds.");
		if (!ever_authed)
			g_app.screen = ST_ERROR;
		retry_at = osGetTime() + RETRY_INTERVAL_MS;
	}
}

static void begin_auth(void)
{
	client_reset();
	client_auth();
	last_ping = osGetTime();
	snprintf(g_app.status, sizeof g_app.status, "Logging in to the bridge...");
}

// Called once the background connect has finished.
static void connect_finished(int result, const unsigned char fp[32], const char *err)
{
	if (result < 0) {
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
	net_close(); // also cancels a connect attempt still running
	client_reset();
	retry_at = 0;
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

static void search_chats(void)
{
	SwkbdState kb;
	char buf[64];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof buf - 1);
	swkbdSetHintText(&kb, "Search chats by name or number");
	if (g_app.search[0])
		swkbdSetInitialText(&kb, g_app.search);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Search", true);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM)
		return;
	size_t n = strlen(buf);
	while (n > 0 && buf[n - 1] == ' ')
		buf[--n] = 0;
	strlcpy_utf8(g_app.search, buf, sizeof g_app.search);
	g_app.chat_sel = 0;
	g_app.chat_scroll = 0;
	chats_filter();
	g_app.chat_sel = 0;
}

static void clear_search(void)
{
	g_app.search[0] = 0;
	chats_filter();
}

static void input_chats(u32 down, u32 repeat, u32 held)
{
	if (down & KEY_SELECT) {
		run_setup();
		return;
	}
	if (down & KEY_X)
		client_request_chats();
	if (down & KEY_Y) {
		search_chats();
		return;
	}
	if (down & KEY_B && g_app.search[0]) {
		clear_search();
		return;
	}

	int n = g_app.nvis;
	if (repeat & KEY_DOWN && g_app.chat_sel < n - 1)
		g_app.chat_sel++;
	if (repeat & KEY_UP && g_app.chat_sel > 0)
		g_app.chat_sel--;
	if (repeat & KEY_RIGHT)
		g_app.chat_sel = g_app.chat_sel + 5 < n ? g_app.chat_sel + 5 : n - 1;
	if (repeat & KEY_LEFT)
		g_app.chat_sel = g_app.chat_sel - 5 > 0 ? g_app.chat_sel - 5 : 0;
	if (g_app.chat_sel < 0)
		g_app.chat_sel = 0;

	float list_h = SCR_H - 24;
	float max_scroll = n * 44.0f - list_h;
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

	// touch: drag to scroll, tap a row to open, tap the header to search
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
			if (touch_start.py < 24) {
				if (g_app.search[0] && touch_start.px > BOT_W - 70)
					clear_search();
				else
					search_chats();
				return;
			}
			int row = ui_chat_row_at(touch_start.py);
			if (row >= 0) {
				g_app.chat_sel = row;
				client_open_chat(chat_at(row));
				return;
			}
		}
	}

	if (g_app.chat_scroll > max_scroll)
		g_app.chat_scroll = max_scroll;
	if (g_app.chat_scroll < 0)
		g_app.chat_scroll = 0;

	if (down & KEY_A && chat_at(g_app.chat_sel))
		client_open_chat(chat_at(g_app.chat_sel));
}

static void leave_chat(void)
{
	g_app.screen = ST_CHATS;
	msgs_clear();
	g_app.cur_jid[0] = 0;
}

static void open_camera(void)
{
	char err[96];
	if (!cam_open(false, err, sizeof err)) {
		ui_show_toast(err, 4);
		return;
	}
	g_app.caption[0] = 0;
	g_app.upload_result = 0;
	g_app.screen = ST_CAMERA;
}

static void open_voice(void)
{
	if (g_app.call_state != CALL_NONE && g_app.call_state != CALL_ENDED) {
		ui_show_toast("Can't record during a call", 3);
		return;
	}
	if (!audio_available()) {
		ui_show_toast("No sound: dump the DSP firmware with DSP1 first", 5);
		return;
	}
	audio_clip_stop();
	audio_record_discard();
	g_app.upload_result = 0;
	g_app.screen = ST_VOICE;
}

static void chat_action(int action)
{
	const Msg *m = (g_app.msg_sel >= 0 && g_app.msg_sel < g_app.nmsgs) ? &g_app.msgs[g_app.msg_sel] : NULL;
	switch (action) {
	case 0:
		write_message();
		break;
	case 1:
		open_camera();
		break;
	case 2:
		open_voice();
		break;
	case 3:
		if (m && m->has_media) {
			if (!strcmp(m->kind, "audio"))
				client_play_voice(m);
			else
				client_request_image(m);
		}
		break;
	case 4:
		if (!g_app.cur_group)
			client_call_start(g_app.cur_jid);
		else
			ui_show_toast("Group calls are not supported", 3);
		break;
	case 5:
		client_load_older();
		break;
	case 6:
		audio_clip_stop();
		leave_chat();
		break;
	}
}

static void edit_caption(void)
{
	SwkbdState kb;
	char buf[256];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 200);
	swkbdSetHintText(&kb, "Caption (optional)");
	if (g_app.caption[0])
		swkbdSetInitialText(&kb, g_app.caption);
	swkbdSetFeatures(&kb, SWKBD_MULTILINE);
	if (swkbdInputText(&kb, buf, sizeof buf) == SWKBD_BUTTON_CONFIRM)
		strlcpy_utf8(g_app.caption, buf, sizeof g_app.caption);
}

static void input_camera(u32 down)
{
	if (g_app.upload_result > 0) { // sent
		cam_close();
		ui_show_toast("Photo sent", 3);
		g_app.upload_result = 0;
		g_app.screen = ST_CHAT;
		return;
	}
	if (g_app.uploading)
		return;
	int b = -1;
	if (down & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		b = ui_compose_button_at(tp.px, tp.py);
	}
	if (!cam_frozen()) {
		if (down & KEY_A || b == 1) {
			if (!cam_snap())
				ui_show_toast("The camera isn't ready yet", 2);
		} else if (down & KEY_X || b == 0) {
			cam_switch();
		} else if (down & KEY_B || b == 2) {
			cam_close();
			g_app.screen = ST_CHAT;
		}
		return;
	}
	if (down & KEY_A || b == 1) {
		unsigned char *jpeg = NULL;
		unsigned long len = 0;
		if (!cam_jpeg(&jpeg, &len, 85)) {
			ui_show_toast("Could not encode the photo", 3);
			return;
		}
		client_send_image(jpeg, len, g_app.caption);
		free(jpeg); // the send queue has its own copy
	} else if (down & KEY_Y || b == 0) {
		edit_caption();
	} else if (down & KEY_B || b == 2) {
		g_app.upload_result = 0;
		cam_retake();
	}
}

static void input_voice(u32 down)
{
	if (g_app.upload_result > 0) { // sent
		audio_record_discard();
		ui_show_toast("Voice message sent", 3);
		g_app.upload_result = 0;
		g_app.screen = ST_CHAT;
		return;
	}
	if (g_app.uploading)
		return;
	int b = -1;
	if (down & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		b = ui_compose_button_at(tp.px, tp.py);
	}
	size_t len;
	const unsigned char *data = audio_record_data(&len);

	if (audio_recording()) {
		if (down & KEY_A || b == 1)
			audio_record_stop();
		else if (down & KEY_B || b == 2)
			audio_record_discard();
		return;
	}
	if (len > 0) {
		if (down & KEY_A || b == 1) {
			audio_clip_stop();
			if (audio_record_ms() < 500)
				ui_show_toast("Too short - hold on a bit longer", 3);
			else
				client_send_voice(data, len);
		} else if (down & KEY_Y || b == 0) {
			if (audio_clip_playing()) {
				audio_clip_stop();
			} else {
				unsigned char *copy = malloc(len); // the player takes ownership
				if (copy) {
					memcpy(copy, data, len);
					audio_clip_play(copy, len);
				}
			}
		} else if (down & KEY_B || b == 2) {
			audio_clip_stop();
			audio_record_discard();
			g_app.upload_result = 0;
		}
		return;
	}
	if (down & KEY_A || b == 1) {
		g_app.upload_result = 0;
		if (!audio_record_start())
			ui_show_toast("Microphone unavailable", 3);
	} else if (down & KEY_B || b == 2) {
		g_app.screen = ST_CHAT;
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
	else if (down & KEY_R)
		chat_action(1);
	else if (down & KEY_SELECT)
		chat_action(2);
	else if (down & KEY_Y)
		chat_action(3);
	else if (down & KEY_X)
		chat_action(4);
	else if (down & KEY_L)
		chat_action(5);
	else if (down & KEY_B)
		chat_action(6);
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

static void input_call(u32 down)
{
	int b = -1;
	if (down & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		b = ui_call_button_at(tp.px, tp.py);
	}
	switch (g_app.call_state) {
	case CALL_INCOMING:
		if (down & KEY_A || b == 1)
			client_call_accept();
		else if (down & KEY_B || b == 0)
			client_call_hangup();
		break;
	case CALL_RINGING:
	case CALL_CONNECTING:
		if (down & KEY_B || b == 1)
			client_call_hangup();
		break;
	case CALL_ACTIVE:
		if (down & KEY_B || b == 1)
			client_call_hangup();
		else if (down & KEY_X || b == 0)
			g_app.call_muted = !g_app.call_muted;
		break;
	default:
		break;
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
	audio_init(); // without DSP firmware, calls still connect but are silent
	g_app.stick_bottom = true;

	if (config_load()) {
		start_connect();
	} else {
		g_app.screen = ST_SETUP;
	}

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), repeat = hidKeysDownRepeat(), held = hidKeysHeld();
		if (down & KEY_START) {
			if (g_app.screen == ST_CALL && g_app.call_state != CALL_ENDED) {
				client_call_hangup();
				continue;
			}
			break;
		}

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
			if (net_connecting())
				break;
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
		case ST_CALL:
			input_call(down);
			break;
		case ST_CAMERA:
			input_camera(down);
			break;
		case ST_VOICE:
			input_voice(down);
			break;
		}

		// an incoming call takes over the camera/recorder: release the hardware
		if (g_app.screen == ST_CALL)
			cam_close(); // no-op when the camera is already off
		if (g_app.screen == ST_CAMERA)
			cam_update();

		// networking
		if (net_connecting()) {
			unsigned char fp[32];
			char err[128];
			int r = net_connect_poll(fp, err, sizeof err);
			if (r != 0)
				connect_finished(r, fp, err);
		}

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
		} else if (ever_authed && retry_at && osGetTime() >= retry_at && !net_connecting()) {
			start_connect(); // background reconnect, stays on the current screen
		}

		audio_update();
		client_tick();
		ui_render();
	}

	if (g_app.call_state != CALL_NONE && g_app.call_state != CALL_ENDED)
		client_call_hangup();
	cam_close();
	audio_exit();
	net_exit();
	ui_exit();
	gfxExit();
	return 0;
}
