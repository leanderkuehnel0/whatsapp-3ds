// Chat list and conversation state, plus small string/time helpers.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

App g_app;

Chat *chat_find(const char *jid)
{
	for (int i = 0; i < g_app.nchats; i++)
		if (strcmp(g_app.chats[i].jid, jid) == 0)
			return &g_app.chats[i];
	return NULL;
}

static int chat_cmp(const void *a, const void *b)
{
	s64 ta = ((const Chat *)a)->ts, tb = ((const Chat *)b)->ts;
	return ta < tb ? 1 : ta > tb ? -1 : 0;
}

Chat *chat_at(int i)
{
	// the list may be mid-refresh (nchats reset while vis is stale): check both bounds
	if (i < 0 || i >= g_app.nvis || g_app.vis[i] >= g_app.nchats)
		return NULL;
	return &g_app.chats[g_app.vis[i]];
}

// Appends the case-folded UTF-8 of s to out (ASCII and Latin-1 letters, so "ö" matches "Ö").
static void fold(const char *s, char *out, size_t n)
{
	size_t o = 0;
	while (*s && o + 4 < n) {
		u32 cp;
		ssize_t len = decode_utf8(&cp, (const uint8_t *)s);
		if (len <= 0) {
			s++;
			continue;
		}
		s += len;
		if (cp >= 'A' && cp <= 'Z')
			cp += 32;
		else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
			cp += 32;
		o += encode_utf8((uint8_t *)out + o, cp);
	}
	out[o] = 0;
}

static bool chat_matches(const Chat *c, const char *needle)
{
	char hay[200];
	fold(c->name, hay, sizeof hay);
	if (strstr(hay, needle))
		return true;
	// also match phone numbers, ignoring a leading "+"
	const char *n = needle[0] == '+' ? needle + 1 : needle;
	return n[0] >= '0' && n[0] <= '9' && strstr(c->jid, n) != NULL;
}

void chats_filter(void)
{
	const Chat *sel = chat_at(g_app.chat_sel);
	char seljid[72] = "";
	if (sel)
		strcpy(seljid, sel->jid);

	char needle[128];
	fold(g_app.search, needle, sizeof needle);
	g_app.nvis = 0;
	for (int i = 0; i < g_app.nchats; i++)
		if (!needle[0] || chat_matches(&g_app.chats[i], needle))
			g_app.vis[g_app.nvis++] = i;

	g_app.chat_sel = 0;
	for (int i = 0; i < g_app.nvis; i++)
		if (!strcmp(g_app.chats[g_app.vis[i]].jid, seljid))
			g_app.chat_sel = i;
}

void chats_sort(void)
{
	// keep the selection on the same chat across reordering
	const Chat *sel = chat_at(g_app.chat_sel);
	char seljid[72] = "";
	if (sel)
		strcpy(seljid, sel->jid);
	qsort(g_app.chats, g_app.nchats, sizeof(Chat), chat_cmp);
	g_app.nvis = 0;
	chats_filter();
	for (int i = 0; i < g_app.nvis; i++)
		if (!strcmp(g_app.chats[g_app.vis[i]].jid, seljid))
			g_app.chat_sel = i;
}

static void msg_free(Msg *m)
{
	free(m->text);
	free(m->wrapped);
	m->text = m->wrapped = NULL;
}

void msgs_clear(void)
{
	for (int i = 0; i < g_app.nmsgs; i++)
		msg_free(&g_app.msgs[i]);
	g_app.nmsgs = 0;
	g_app.msg_sel = 0;
	g_app.view_top = 0;
	g_app.content_h = 0;
	g_app.stick_bottom = true;
	g_app.no_older = false;
}

Msg *msg_find(const char *id)
{
	for (int i = 0; i < g_app.nmsgs; i++)
		if (strcmp(g_app.msgs[i].id, id) == 0)
			return &g_app.msgs[i];
	return NULL;
}

static const char *display_text(const Msg *m)
{
	if (m->text && m->text[0])
		return m->text;
	if (!strcmp(m->kind, "image"))
		return "[Photo]";
	if (!strcmp(m->kind, "sticker"))
		return "[Sticker]";
	return "";
}

void msg_set_text(Msg *m, const char *text)
{
	free(m->text);
	free(m->wrapped);
	m->text = strdup(text ? text : "");

	// prefix media messages so it's clear an image can be opened
	const char *shown = display_text(m);
	char *tmp = NULL;
	if (m->has_media && (!strcmp(m->kind, "image") || !strcmp(m->kind, "video")) && m->text[0]) {
		const char *label = !strcmp(m->kind, "image") ? "[Photo] " : "";
		tmp = malloc(strlen(label) + strlen(shown) + 1);
		strcpy(tmp, label);
		strcat(tmp, shown);
		shown = tmp;
	}
	m->wrapped = ui_wrap(shown, BUBBLE_MAX_W - 12, MSG_SCALE, &m->lines, &m->text_w);
	free(tmp);
}

// Bubble geometry: padding, optional sender line, text lines, timestamp line.
#define BUBBLE_PAD 4.0f
#define BUBBLE_GAP 4.0f
#define SMALL_LINE 12.0f

void msgs_layout(void)
{
	float y = 4;
	for (int i = 0; i < g_app.nmsgs; i++) {
		Msg *m = &g_app.msgs[i];
		float h = BUBBLE_PAD * 2 + m->lines * LINE_H + SMALL_LINE;
		if (g_app.cur_group && !m->from_me)
			h += SMALL_LINE;
		m->y = y;
		m->h = h;
		y += h + BUBBLE_GAP;
	}
	g_app.content_h = y;
}

void fmt_time(s64 ts, char *out, size_t n, bool with_date)
{
	time_t local = (time_t)(ts + g_app.clock_offset);
	time_t now = time(NULL);
	struct tm t, tn;
	gmtime_r(&local, &t);
	gmtime_r(&now, &tn);
	bool today = t.tm_year == tn.tm_year && t.tm_yday == tn.tm_yday;
	if (with_date || !today)
		snprintf(out, n, "%02d.%02d.%02d %02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_year % 100, t.tm_hour, t.tm_min);
	else
		snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

// Copies at most n-1 bytes without splitting a UTF-8 sequence.
void strlcpy_utf8(char *dst, const char *src, size_t n)
{
	if (n == 0)
		return;
	size_t len = strlen(src);
	if (len >= n) {
		len = n - 1;
		while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80)
			len--;
	}
	memcpy(dst, src, len);
	dst[len] = 0;
}
