// Settings stored in sdmc:/3ds/wa3ds/config.ini as key=value lines.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Config g_cfg = {.port = 7473};

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
	size_t i = 0;
	while (s[i] == ' ' || s[i] == '\t')
		i++;
	if (i)
		memmove(s, s + i, n - i + 1);
}

bool config_load(void)
{
	FILE *f = fopen(CONFIG_PATH, "r");
	if (!f)
		return false;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		char *eq = strchr(ln, '=');
		if (!eq || ln[0] == '#')
			continue;
		*eq = 0;
		char *key = ln, *val = eq + 1;
		trim(key);
		trim(val);
		if (!strcmp(key, "host"))
			strlcpy_utf8(g_cfg.host, val, sizeof g_cfg.host);
		else if (!strcmp(key, "port"))
			g_cfg.port = atoi(val) > 0 ? atoi(val) : 7473;
		else if (!strcmp(key, "token"))
			strlcpy_utf8(g_cfg.token, val, sizeof g_cfg.token);
		else if (!strcmp(key, "pin"))
			strlcpy_utf8(g_cfg.pin, val, sizeof g_cfg.pin);
	}
	fclose(f);
	return g_cfg.host[0] && g_cfg.token[0];
}

bool config_save(void)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(CONFIG_DIR, 0777);
	FILE *f = fopen(CONFIG_PATH, "w");
	if (!f)
		return false;
	fprintf(f, "# WA3DS settings. Delete pin= to trust a new bridge certificate.\n");
	fprintf(f, "host=%s\nport=%d\ntoken=%s\npin=%s\n", g_cfg.host, g_cfg.port, g_cfg.token, g_cfg.pin);
	fclose(f);
	return true;
}

static bool keyboard(SwkbdType type, const char *hint, const char *initial, char *out, size_t n)
{
	SwkbdState kb;
	char buf[256];
	swkbdInit(&kb, type, 2, (int)n - 1);
	swkbdSetHintText(&kb, hint);
	if (initial && initial[0])
		swkbdSetInitialText(&kb, initial);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (type == SWKBD_TYPE_NUMPAD)
		swkbdSetNumpadKeys(&kb, 0, 0);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM)
		return false;
	trim(buf);
	strlcpy_utf8(out, buf, n);
	return true;
}

bool config_prompt(void)
{
	Config c = g_cfg;
	char port[8];
	snprintf(port, sizeof port, "%d", c.port ? c.port : 7473);

	if (!keyboard(SWKBD_TYPE_NORMAL, "Bridge address (domain or IP)", c.host, c.host, sizeof c.host))
		return false;
	if (!keyboard(SWKBD_TYPE_NUMPAD, "Bridge port", port, port, sizeof port))
		return false;
	if (!keyboard(SWKBD_TYPE_NORMAL, "Token printed by the bridge", c.token, c.token, sizeof c.token))
		return false;

	c.port = atoi(port) > 0 ? atoi(port) : 7473;
	// a different server means a different certificate
	if (strcmp(c.host, g_cfg.host) != 0 || c.port != g_cfg.port)
		c.pin[0] = 0;
	g_cfg = c;
	config_save();
	return true;
}
