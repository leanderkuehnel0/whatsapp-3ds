// Protocol client: sends requests and applies responses/events to g_app.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { RQ_NONE, RQ_AUTH, RQ_CHATS, RQ_MSGS, RQ_OLDER, RQ_SEND, RQ_READ, RQ_MEDIA, RQ_PING, RQ_CALL, RQ_CALLCTL } ReqType;

typedef struct {
	int     tag;
	ReqType type;
	char    jid[72];
	char    id[72];
	// buffered results for MSGS so they can be applied in one go
	int     count;
} Pending;

#define MAX_PENDING 32
static Pending pending[MAX_PENDING];
static int next_tag = 1;

// Older messages arrive oldest-first and are collected here before being prepended.
static Msg older_buf[100];
static int older_n;

static Pending *pending_add(ReqType type, const char *jid, const char *id)
{
	for (int i = 0; i < MAX_PENDING; i++) {
		if (pending[i].type == RQ_NONE) {
			Pending *p = &pending[i];
			memset(p, 0, sizeof *p);
			p->tag = next_tag++;
			p->type = type;
			if (jid)
				strlcpy_utf8(p->jid, jid, sizeof p->jid);
			if (id)
				strlcpy_utf8(p->id, id, sizeof p->id);
			return p;
		}
	}
	return NULL;
}

static Pending *pending_get(const char *tag)
{
	int t = atoi(tag);
	for (int i = 0; i < MAX_PENDING; i++)
		if (pending[i].type != RQ_NONE && pending[i].tag == t)
			return &pending[i];
	return NULL;
}

void client_reset(void)
{
	memset(pending, 0, sizeof pending);
	for (int i = 0; i < older_n; i++) {
		free(older_buf[i].text);
		free(older_buf[i].wrapped);
	}
	older_n = 0;
	g_app.authed = false;
	g_app.loading_msgs = false;
	g_app.img_loading = false;
}

static bool request(ReqType type, const char *jid, const char *id, const char **args, int nargs)
{
	Pending *p = pending_add(type, jid, id);
	if (!p)
		return false;
	char tag[12];
	snprintf(tag, sizeof tag, "%d", p->tag);
	const char *f[8];
	f[0] = tag;
	for (int i = 0; i < nargs && i < 7; i++)
		f[i + 1] = args[i];
	if (!net_send(f, nargs + 1)) {
		p->type = RQ_NONE;
		return false;
	}
	return true;
}

void client_auth(void)
{
	const char *a[] = {"AUTH", g_cfg.token};
	request(RQ_AUTH, NULL, NULL, a, 2);
}

void client_ping(void)
{
	const char *a[] = {"PING"};
	request(RQ_PING, NULL, NULL, a, 1);
}

void client_request_chats(void)
{
	const char *a[] = {"CHATS"};
	request(RQ_CHATS, NULL, NULL, a, 1);
}

void client_open_chat(const Chat *c)
{
	msgs_clear();
	strlcpy_utf8(g_app.cur_jid, c->jid, sizeof g_app.cur_jid);
	strlcpy_utf8(g_app.cur_name, c->name, sizeof g_app.cur_name);
	g_app.cur_group = c->group;
	g_app.screen = ST_CHAT;
	g_app.loading_msgs = true;
	const char *a[] = {"MSGS", c->jid, "0", "40"};
	request(RQ_MSGS, c->jid, NULL, a, 4);
	client_mark_read();
}

void client_load_older(void)
{
	if (g_app.loading_msgs || g_app.no_older || g_app.nmsgs == 0)
		return;
	char before[24];
	snprintf(before, sizeof before, "%lld", (long long)g_app.msgs[0].ts);
	const char *a[] = {"MSGS", g_app.cur_jid, before, "30"};
	older_n = 0;
	if (request(RQ_OLDER, g_app.cur_jid, NULL, a, 4))
		g_app.loading_msgs = true;
}

void client_send_text(const char *text)
{
	const char *a[] = {"SEND", g_app.cur_jid, text};
	if (!request(RQ_SEND, g_app.cur_jid, NULL, a, 3))
		ui_show_toast("Not connected - message not sent", 4);
}

void client_mark_read(void)
{
	Chat *c = chat_find(g_app.cur_jid);
	if (c)
		c->unread = 0;
	const char *a[] = {"READ", g_app.cur_jid};
	request(RQ_READ, g_app.cur_jid, NULL, a, 2);
}

void client_request_image(const Msg *m)
{
	image_free();
	g_app.img_ready = false;
	g_app.img_err[0] = 0;
	g_app.img_loading = true;
	g_app.screen = ST_IMAGE;
	const char *a[] = {"MEDIA", g_app.cur_jid, m->id};
	if (!request(RQ_MEDIA, g_app.cur_jid, m->id, a, 3)) {
		g_app.img_loading = false;
		strcpy(g_app.img_err, "Not connected");
	}
}

// ---------------------------------------------------------------- calls

// After a reconnect the bridge re-sends CALLSTATE for a call that is still going.
// If none arrives shortly, the call ended while we were offline.
static u64 call_check_until;

static void call_enter_screen(void)
{
	if (g_app.screen != ST_CALL) {
		g_app.call_prev_screen = g_app.screen == ST_IMAGE ? ST_CHAT : g_app.screen;
		g_app.screen = ST_CALL;
	}
}

static void call_set_state(CallState cs)
{
	CallState prev = g_app.call_state;
	g_app.call_state = cs;
	switch (cs) {
	case CALL_RINGING:
		audio_ring(2);
		break;
	case CALL_INCOMING:
		audio_ring(1);
		break;
	case CALL_CONNECTING:
		audio_ring(0);
		break;
	case CALL_ACTIVE:
		if (prev != CALL_ACTIVE) {
			g_app.call_started = osGetTime();
			if (!audio_call_begin())
				ui_show_toast("Microphone unavailable - the other side won't hear you", 6);
		}
		break;
	case CALL_ENDED:
	case CALL_NONE:
		audio_call_end();
		g_app.call_ended_at = osGetTime();
		break;
	}
}

void client_call_start(const char *jid)
{
	if (g_app.call_state != CALL_NONE && g_app.call_state != CALL_ENDED)
		return;
	if (!audio_available())
		ui_show_toast("No sound: dump the DSP firmware with DSP1 first", 6);
	Chat *c = chat_find(jid);
	g_app.call_id[0] = 0;
	strlcpy_utf8(g_app.call_jid, jid, sizeof g_app.call_jid);
	strlcpy_utf8(g_app.call_name, c ? c->name : jid, sizeof g_app.call_name);
	g_app.call_incoming = false;
	g_app.call_muted = false;
	g_app.call_end_reason[0] = 0;
	call_enter_screen();
	call_set_state(CALL_RINGING);
	const char *a[] = {"CALL", jid};
	if (!request(RQ_CALL, jid, NULL, a, 2)) {
		strcpy(g_app.call_end_reason, "not connected");
		call_set_state(CALL_ENDED);
	}
}

void client_call_accept(void)
{
	if (g_app.call_state != CALL_INCOMING || !g_app.call_id[0])
		return;
	const char *a[] = {"ACCEPT", g_app.call_id};
	request(RQ_CALLCTL, NULL, NULL, a, 2);
	call_set_state(CALL_CONNECTING);
}

void client_call_hangup(void)
{
	if (g_app.call_id[0]) {
		const char *a[] = {"HANGUP", g_app.call_id};
		request(RQ_CALLCTL, NULL, NULL, a, 2);
	}
	call_set_state(CALL_ENDED);
}

// * CALLSTATE <id> <chat> <name> <state> <in/out> <reason>
static void on_call_state(char **f, int n)
{
	if (n < 6)
		return;
	const char *id = f[0], *st = f[3];
	bool busy = g_app.call_state != CALL_NONE && g_app.call_state != CALL_ENDED;
	if (busy && g_app.call_id[0] && strcmp(g_app.call_id, id) != 0)
		return; // some other call; the bridge only handles one at a time anyway
	call_check_until = 0;

	CallState cs = CALL_ENDED;
	if (!strcmp(st, "ringing"))
		cs = CALL_RINGING;
	else if (!strcmp(st, "incoming"))
		cs = CALL_INCOMING;
	else if (!strcmp(st, "connecting"))
		cs = CALL_CONNECTING;
	else if (!strcmp(st, "active"))
		cs = CALL_ACTIVE;

	if (cs == CALL_ENDED && g_app.screen != ST_CALL) {
		g_app.call_state = CALL_NONE;
		return;
	}
	if (!busy) {
		g_app.call_muted = false;
		g_app.call_end_reason[0] = 0;
	}
	strlcpy_utf8(g_app.call_id, id, sizeof g_app.call_id);
	strlcpy_utf8(g_app.call_jid, f[1], sizeof g_app.call_jid);
	Chat *c = chat_find(f[1]);
	strlcpy_utf8(g_app.call_name, c ? c->name : f[2], sizeof g_app.call_name);
	g_app.call_incoming = !strcmp(f[4], "in");
	strlcpy_utf8(g_app.call_end_reason, f[5], sizeof g_app.call_end_reason);
	if (cs != CALL_ENDED)
		call_enter_screen();
	if (cs != g_app.call_state)
		call_set_state(cs);
}

void client_tick(void)
{
	if (call_check_until && osGetTime() > call_check_until) {
		call_check_until = 0;
		if (g_app.call_state != CALL_NONE && g_app.call_state != CALL_ENDED) {
			strcpy(g_app.call_end_reason, "connection lost");
			call_set_state(CALL_ENDED);
		}
	}
	// leave the call screen a moment after the call is over
	if (g_app.screen == ST_CALL && (g_app.call_state == CALL_ENDED || g_app.call_state == CALL_NONE) &&
	    osGetTime() - g_app.call_ended_at > 2500) {
		g_app.screen = g_app.call_prev_screen;
		if (g_app.screen == ST_CALL || g_app.screen == ST_IMAGE)
			g_app.screen = ST_CHATS;
		if (g_app.screen == ST_CHAT && !g_app.cur_jid[0])
			g_app.screen = ST_CHATS;
		g_app.call_state = CALL_NONE;
		g_app.call_id[0] = 0;
	}
}

// ---------------------------------------------------------------- parsing

// <chat> = jid name is_group last_ts unread preview
static void parse_chat(char **f, int n, Chat *c)
{
	memset(c, 0, sizeof *c);
	if (n < 6)
		return;
	strlcpy_utf8(c->jid, f[0], sizeof c->jid);
	strlcpy_utf8(c->name, f[1], sizeof c->name);
	c->group = f[2][0] == '1';
	c->ts = strtoll(f[3], NULL, 10);
	c->unread = atoi(f[4]);
	strlcpy_utf8(c->preview, f[5], sizeof c->preview);
}

static void upsert_chat(char **f, int n)
{
	Chat c;
	parse_chat(f, n, &c);
	if (!c.jid[0])
		return;
	Chat *dst = chat_find(c.jid);
	if (!dst) {
		if (g_app.nchats >= MAX_CHATS)
			return;
		dst = &g_app.chats[g_app.nchats++];
	}
	// the open chat is being read right now
	if (g_app.screen != ST_CHATS && !strcmp(c.jid, g_app.cur_jid))
		c.unread = 0;
	*dst = c;
}

// <msg> = chat_jid msg_id ts from_me sender_name kind has_media text
static bool parse_msg(char **f, int n, char *chat_out, size_t chat_n, Msg *m)
{
	memset(m, 0, sizeof *m);
	if (n < 8)
		return false;
	strlcpy_utf8(chat_out, f[0], chat_n);
	strlcpy_utf8(m->id, f[1], sizeof m->id);
	m->ts = strtoll(f[2], NULL, 10);
	m->from_me = f[3][0] == '1';
	strlcpy_utf8(m->sender, f[4], sizeof m->sender);
	strlcpy_utf8(m->kind, f[5], sizeof m->kind);
	m->has_media = f[6][0] == '1';
	msg_set_text(m, f[7]);
	return true;
}

static void append_msg(Msg *m)
{
	if (g_app.nmsgs >= MAX_MSGS) {
		// drop the oldest to make room
		free(g_app.msgs[0].text);
		free(g_app.msgs[0].wrapped);
		memmove(&g_app.msgs[0], &g_app.msgs[1], sizeof(Msg) * (MAX_MSGS - 1));
		g_app.nmsgs--;
		if (g_app.msg_sel > 0)
			g_app.msg_sel--;
	}
	g_app.msgs[g_app.nmsgs++] = *m;
}

static void scroll_to_bottom(void)
{
	msgs_layout();
	if (g_app.nmsgs > 0)
		g_app.msg_sel = g_app.nmsgs - 1;
	float max_top = g_app.content_h - (SCR_H - HEADER_H);
	g_app.view_top = max_top > 0 ? max_top : 0;
	g_app.stick_bottom = true;
}

static void on_event_msg(char **f, int n)
{
	char chat[72];
	Msg m;
	if (!parse_msg(f, n, chat, sizeof chat, &m))
		return;

	bool in_chat = (g_app.screen == ST_CHAT || g_app.screen == ST_IMAGE) && !strcmp(chat, g_app.cur_jid);
	if (!in_chat) {
		if (!m.from_me) {
			Chat *c = chat_find(chat);
			char t[200];
			snprintf(t, sizeof t, "%s: %s", c ? c->name : (m.sender[0] ? m.sender : "New message"),
			         m.text[0] ? m.text : "[Media]");
			ui_show_toast(t, 4);
		}
		free(m.text);
		free(m.wrapped);
		return;
	}

	Msg *existing = msg_find(m.id);
	if (existing) {
		// edit or deletion: keep position, replace content
		free(existing->text);
		free(existing->wrapped);
		*existing = m;
		msgs_layout();
		return;
	}
	if (g_app.nmsgs > 0 && m.ts < g_app.msgs[g_app.nmsgs - 1].ts) {
		free(m.text);
		free(m.wrapped);
		return; // out-of-order old message; it will show up on reopen
	}
	bool was_at_end = g_app.stick_bottom || g_app.msg_sel >= g_app.nmsgs - 1;
	append_msg(&m);
	if (was_at_end)
		scroll_to_bottom();
	else
		msgs_layout();
	if (!m.from_me)
		client_mark_read();
}

static void on_event(char **f, int n)
{
	const char *ev = f[1];
	if (!strcmp(ev, "MSG")) {
		on_event_msg(f + 2, n - 2);
	} else if (!strcmp(ev, "CHAT")) {
		upsert_chat(f + 2, n - 2);
		chats_sort();
	} else if (!strcmp(ev, "RELOAD")) {
		client_request_chats();
	} else if (!strcmp(ev, "STATUS") && n >= 3) {
		g_app.wa_online = !strcmp(f[2], "connected");
		if (!strcmp(f[2], "logged_out"))
			ui_show_toast("WhatsApp logged this bridge out. Re-link it on the server.", 10);
	} else if (!strcmp(ev, "CALLSTATE")) {
		on_call_state(f + 2, n - 2);
	}
}

void client_on_line(char **f, int n)
{
	if (n < 2)
		return;
	if (!strcmp(f[0], "*")) {
		on_event(f, n);
		return;
	}
	Pending *p = pending_get(f[0]);
	if (!p)
		return;
	const char *kind = f[1];
	bool done = !strcmp(kind, "OK") || !strcmp(kind, "ERR") || !strcmp(kind, "END");

	switch (p->type) {
	case RQ_AUTH:
		if (!strcmp(kind, "OK")) {
			g_app.authed = true;
			if (n >= 3)
				strlcpy_utf8(g_app.own_name, f[2], sizeof g_app.own_name);
			if (n >= 5) {
				s64 server_now = strtoll(f[4], NULL, 10);
				s64 off = (s64)time(NULL) - server_now;
				// the 3DS clock is local time; round to the nearest 15 minute timezone step
				g_app.clock_offset = ((off + (off >= 0 ? 450 : -450)) / 900) * 900;
			}
			if (g_app.screen == ST_CONNECTING || g_app.screen == ST_ERROR || g_app.screen == ST_TRUST)
				g_app.screen = ST_CHATS;
			client_request_chats();
			if (g_app.call_state != CALL_NONE && g_app.call_state != CALL_ENDED)
				call_check_until = osGetTime() + 3000;
			if (g_app.screen == ST_CHAT && g_app.cur_jid[0]) {
				// reconnected while a conversation was open: reload it
				Chat c = {0};
				strlcpy_utf8(c.jid, g_app.cur_jid, sizeof c.jid);
				strlcpy_utf8(c.name, g_app.cur_name, sizeof c.name);
				c.group = g_app.cur_group;
				client_open_chat(&c);
			}
		} else if (!strcmp(kind, "ERR")) {
			snprintf(g_app.status, sizeof g_app.status,
			         "The bridge rejected the token.\nPress SELECT to enter it again.");
			g_app.screen = ST_ERROR;
		}
		break;

	case RQ_CHATS:
		if (!strcmp(kind, "CHAT")) {
			if (p->count++ == 0)
				g_app.nchats = 0; // first row of a fresh list
			upsert_chat(f + 2, n - 2);
		} else if (!strcmp(kind, "END")) {
			if (p->count == 0)
				g_app.nchats = 0;
			chats_sort();
			if (g_app.chat_sel >= g_app.nchats)
				g_app.chat_sel = g_app.nchats ? g_app.nchats - 1 : 0;
		}
		break;

	case RQ_MSGS:
	case RQ_OLDER: {
		bool stale = strcmp(p->jid, g_app.cur_jid) != 0;
		if (!strcmp(kind, "MSG") && !stale) {
			char chat[72];
			Msg m;
			if (!parse_msg(f + 2, n - 2, chat, sizeof chat, &m))
				break;
			p->count++;
			if (p->type == RQ_MSGS) {
				if (!msg_find(m.id))
					append_msg(&m);
				else {
					free(m.text);
					free(m.wrapped);
				}
			} else if (older_n < (int)(sizeof older_buf / sizeof older_buf[0])) {
				older_buf[older_n++] = m;
			} else {
				free(m.text);
				free(m.wrapped);
			}
		} else if (done) {
			g_app.loading_msgs = false;
			if (stale)
				break;
			if (p->type == RQ_MSGS) {
				scroll_to_bottom();
				if (p->count < 40)
					g_app.no_older = true;
			} else {
				if (older_n == 0)
					g_app.no_older = true;
				// make room, then prepend while keeping the view on the same message
				int room = MAX_MSGS - g_app.nmsgs;
				int add = older_n < room ? older_n : room;
				int skip = older_n - add; // too many: drop the oldest of the batch
				for (int i = 0; i < skip; i++) {
					free(older_buf[i].text);
					free(older_buf[i].wrapped);
				}
				float old_h = g_app.content_h;
				memmove(&g_app.msgs[add], &g_app.msgs[0], sizeof(Msg) * g_app.nmsgs);
				memcpy(&g_app.msgs[0], &older_buf[skip], sizeof(Msg) * add);
				g_app.nmsgs += add;
				g_app.msg_sel += add;
				older_n = 0;
				msgs_layout();
				g_app.view_top += g_app.content_h - old_h;
				if (add > 0)
					g_app.msg_sel = add - 1; // land on the newest of the loaded batch
				ui_ensure_selected_visible();
			}
		}
		break;
	}

	case RQ_SEND:
		if (!strcmp(kind, "ERR")) {
			char t[200];
			snprintf(t, sizeof t, "Send failed: %s", n >= 3 ? f[2] : "unknown error");
			ui_show_toast(t, 5);
		}
		break;

	case RQ_CALL:
		if (!strcmp(kind, "OK") && n >= 3) {
			if (!g_app.call_id[0])
				strlcpy_utf8(g_app.call_id, f[2], sizeof g_app.call_id);
		} else if (!strcmp(kind, "ERR")) {
			snprintf(g_app.call_end_reason, sizeof g_app.call_end_reason, "%s", n >= 3 ? f[2] : "failed");
			if (g_app.screen == ST_CALL)
				call_set_state(CALL_ENDED);
		}
		break;

	case RQ_CALLCTL:
		if (!strcmp(kind, "ERR") && g_app.screen == ST_CALL) {
			snprintf(g_app.call_end_reason, sizeof g_app.call_end_reason, "%s", n >= 3 ? f[2] : "failed");
			call_set_state(CALL_ENDED);
		}
		break;

	case RQ_MEDIA:
		if (!strcmp(kind, "ERR")) {
			g_app.img_loading = false;
			snprintf(g_app.img_err, sizeof g_app.img_err, "%s", n >= 3 ? f[2] : "Could not load image");
		}
		break;

	default:
		break;
	}

	if (done)
		p->type = RQ_NONE;
}

void client_on_bin(const char *tag, const char *kind, int w, int h, unsigned char *data, size_t len)
{
	(void)w;
	(void)h;
	if (!strcmp(kind, "AUD")) {
		audio_play_frame(data, len);
		free(data);
		return;
	}
	Pending *p = pending_get(tag);
	if (p && p->type == RQ_MEDIA) {
		p->type = RQ_NONE;
		g_app.img_loading = false;
		if (g_app.screen == ST_IMAGE) {
			char err[96];
			if (image_set_jpeg(data, len, err, sizeof err))
				g_app.img_ready = true;
			else
				snprintf(g_app.img_err, sizeof g_app.img_err, "%s", err);
		}
	}
	free(data);
}
