// Shared declarations for the WA3DS client.
#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define CONFIG_DIR  "sdmc:/3ds/wa3ds"
#define CONFIG_PATH "sdmc:/3ds/wa3ds/config.ini"

// ---------------------------------------------------------------- config.c
typedef struct {
	char host[128];
	int  port;
	char token[64];
	char pin[65]; // hex SHA-256 of the bridge certificate, empty until trusted
} Config;

extern Config g_cfg;
bool config_load(void);
bool config_save(void);
// Runs the on-screen keyboard to fill in host, port and token. Returns false if cancelled.
bool config_prompt(void);

// ---------------------------------------------------------------- net.c
// Called for each received protocol line, already split into unescaped fields.
typedef void (*LineHandler)(char **fields, int nfields);
// Called when a BIN payload has been fully received. The handler takes ownership of data.
typedef void (*BinHandler)(const char *tag, int w, int h, unsigned char *data, size_t len);

void net_init(void);
void net_exit(void);
// Connects and completes the TLS handshake. fp receives the server certificate's SHA-256.
bool net_connect(const char *host, int port, unsigned char fp[32], char *err, size_t errlen);
void net_close(void);
bool net_connected(void);
// Sends one line made of fields (escaped here). Returns false if the connection broke.
bool net_send(const char **fields, int nfields);
// Reads whatever has arrived and dispatches complete lines. Returns false if the connection broke.
bool net_poll(LineHandler on_line, BinHandler on_bin);
// Bytes received so far of a pending BIN payload (for progress display).
size_t net_bin_progress(size_t *total);

// ---------------------------------------------------------------- model.c
#define MAX_CHATS 200
#define MAX_MSGS  400

typedef struct {
	char jid[72];
	char name[96];
	bool group;
	s64  ts;
	int  unread;
	char preview[200];
} Chat;

typedef struct {
	char  id[72];
	s64   ts;
	bool  from_me;
	bool  has_media;
	char  sender[64];
	char  kind[12];
	char *text;     // owned
	char *wrapped;  // owned, text with line breaks inserted for the bubble width
	float text_w;   // widest wrapped line
	int   lines;
	float y, h;     // layout within the conversation
} Msg;

typedef enum { ST_SETUP, ST_CONNECTING, ST_TRUST, ST_ERROR, ST_CHATS, ST_CHAT, ST_IMAGE } Screen;

typedef struct {
	Screen screen;
	bool   wa_online;      // bridge reports WhatsApp connected
	bool   authed;
	char   own_name[64];
	s64    clock_offset;   // add to server timestamps to get 3DS local time
	char   status[160];    // shown on connecting/error screens

	Chat   chats[MAX_CHATS];
	int    nchats;
	int    chat_sel;
	float  chat_scroll;

	char   cur_jid[72];
	char   cur_name[96];
	bool   cur_group;
	Msg    msgs[MAX_MSGS];
	int    nmsgs;
	int    msg_sel;
	float  view_top;       // first visible pixel row of the conversation
	float  content_h;
	bool   stick_bottom;
	bool   loading_msgs;
	bool   no_older;

	// image viewer
	bool   img_loading;
	bool   img_ready;
	char   img_err[96];

	// pending server certificate during first-time trust
	unsigned char peer_fp[32];
	bool   pin_mismatch;

	char   toast[200];
	u64    toast_until;
} App;

extern App g_app;

Chat *chat_find(const char *jid);
void  chats_sort(void);
void  msgs_clear(void);
Msg  *msg_find(const char *id);
void  msgs_layout(void);
void  msg_set_text(Msg *m, const char *text);

// ---------------------------------------------------------------- client.c
void client_on_line(char **f, int n);
void client_on_bin(const char *tag, int w, int h, unsigned char *data, size_t len);
void client_auth(void);
void client_request_chats(void);
void client_open_chat(const Chat *c);
void client_load_older(void);
void client_send_text(const char *text);
void client_mark_read(void);
void client_request_image(const Msg *m);
void client_ping(void);
void client_reset(void);

// ---------------------------------------------------------------- ui.c
#define TOP_W 400
#define BOT_W 320
#define SCR_H 240
#define HEADER_H 20
#define MSG_SCALE 0.5f
extern float g_line_h; // height of one text line at MSG_SCALE
#define LINE_H g_line_h
#define BUBBLE_MAX_W 300.0f

void  ui_init(void);
void  ui_exit(void);
void  ui_render(void);
// Inserts line breaks so text fits in maxw at the given scale.
char *ui_wrap(const char *text, float maxw, float scale, int *lines, float *widest);
float ui_text_width(const char *text, float scale);
// Bottom-screen hit testing shared with input handling in main.c.
int   ui_chat_row_at(int y);   // index into chats or -1
int   ui_chat_button_at(int x, int y); // 0 write, 1 image, 2 older, 3 back, -1 none
void  ui_show_toast(const char *text, int seconds);
void  ui_ensure_selected_visible(void);

// ---------------------------------------------------------------- image.c
bool  image_set_jpeg(const unsigned char *data, size_t len, char *err, size_t errlen);
void  image_draw(float x, float y, float maxw, float maxh);
void  image_free(void);

// ---------------------------------------------------------------- util
void  fmt_time(s64 ts, char *out, size_t n, bool with_date);
void  strlcpy_utf8(char *dst, const char *src, size_t n);
