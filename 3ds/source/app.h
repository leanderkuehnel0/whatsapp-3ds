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
// kind is "BIN" (image) or "AUD" (call audio).
typedef void (*BinHandler)(const char *tag, const char *kind, int w, int h, unsigned char *data, size_t len);

void net_init(void);
void net_exit(void);
// Starts connecting (DNS, TCP, TLS handshake) on a background thread.
bool net_connect_begin(const char *host, int port);
bool net_connecting(void);
// Checks the background connect: 0 still running, 1 connected (fp = server certificate SHA-256),
// -1 failed (err filled in). Call every frame while net_connecting().
int  net_connect_poll(unsigned char fp[32], char *err, size_t errlen);
void net_close(void); // also cancels a connect in progress
bool net_connected(void);
// Sends one line made of fields (escaped here). Returns false if the connection broke.
bool net_send(const char **fields, int nfields);
// Sends "<tag> <kind> <len>" followed by raw bytes; used for call audio (dropped when backlogged).
bool net_send_binary(const char *tag, const char *kind, const unsigned char *data, size_t len);
// Sends a request line followed by a payload (the length must be one of the fields).
bool net_send_payload(const char **fields, int nfields, const unsigned char *data, size_t len);
// Progress of the last payload upload; returns true while it is still being sent.
bool net_upload_progress(size_t *sent, size_t *total);
// Reads whatever has arrived and dispatches complete lines. Returns false if the connection broke.
bool net_poll(LineHandler on_line, BinHandler on_bin);
// Bytes received so far of a pending BIN payload (for progress display).
size_t net_bin_progress(size_t *total);

// ---------------------------------------------------------------- model.c
#define MAX_CHATS 600
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

typedef enum { ST_SETUP, ST_CONNECTING, ST_TRUST, ST_ERROR, ST_CHATS, ST_CHAT, ST_IMAGE, ST_CALL, ST_CAMERA, ST_VOICE } Screen;

typedef enum { CALL_NONE, CALL_RINGING, CALL_INCOMING, CALL_CONNECTING, CALL_ACTIVE, CALL_ENDED } CallState;

typedef struct {
	Screen screen;
	bool   wa_online;      // bridge reports WhatsApp connected
	bool   authed;
	char   own_name[64];
	s64    clock_offset;   // add to server timestamps to get 3DS local time
	char   status[160];    // shown on connecting/error screens

	Chat   chats[MAX_CHATS];
	int    nchats;
	// the list as shown: indices into chats matching the search (all chats when no search)
	int    vis[MAX_CHATS];
	int    nvis;
	char   search[64];
	int    chat_sel;      // index into vis
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

	// voice call
	CallState call_state;
	char   call_id[48];
	char   call_jid[72];
	char   call_name[96];
	bool   call_incoming;
	bool   call_muted;
	u64    call_started;   // when it became active
	u64    call_ended_at;
	char   call_end_reason[32];
	Screen call_prev_screen;

	// photo / voice-message composer
	char   caption[256];
	bool   uploading;        // a photo or voice note is being sent
	int    upload_result;    // 0 pending/none, 1 sent, -1 failed (see upload_err)
	char   upload_err[96];

	// voice-message playback in a chat
	bool   voice_loading;
	char   voice_id[72];     // message being loaded or played
} App;

extern App g_app;

Chat *chat_find(const char *jid);
void  chats_sort(void);   // sorts newest first and re-applies the search filter
void  chats_filter(void); // re-applies the search filter, keeping the selection
Chat *chat_at(int i);     // i-th chat in the shown list, or NULL
void  msgs_clear(void);
Msg  *msg_find(const char *id);
void  msgs_layout(void);
void  msg_set_text(Msg *m, const char *text);

// ---------------------------------------------------------------- client.c
void client_on_line(char **f, int n);
void client_on_bin(const char *tag, const char *kind, int w, int h, unsigned char *data, size_t len);
void client_call_start(const char *jid);
void client_call_accept(void);
void client_call_hangup(void);
void client_tick(void); // per-frame housekeeping (call timeouts)
void client_send_image(const unsigned char *jpeg, size_t len, const char *caption);
void client_send_voice(const unsigned char *adpcm, size_t len);
void client_play_voice(const Msg *m); // toggles: stops if this message is already playing
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
// chat buttons: 0 write, 1 photo, 2 voice, 3 view/play, 4 call, 5 older, 6 back, -1 none
int   ui_chat_button_at(int x, int y);
int   ui_call_button_at(int x, int y); // 0 left, 1 right, -1 none
int   ui_compose_button_at(int x, int y); // camera/voice screens: 0 left, 1 middle, 2 right, -1 none
void  ui_show_toast(const char *text, int seconds);
void  ui_ensure_selected_visible(void);

// ---------------------------------------------------------------- image.c
bool  image_set_jpeg(const unsigned char *data, size_t len, char *err, size_t errlen);
void  image_draw(float x, float y, float maxw, float maxh);
void  image_free(void);

// ---------------------------------------------------------------- audio.c
#define CALL_RATE  16000
#define CALL_FRAME 320 // 20 ms

bool  audio_init(void);          // false if the DSP firmware is missing
bool  audio_available(void);
void  audio_exit(void);
void  audio_ring(int kind);      // 0 off, 1 incoming ring, 2 outgoing ringback
bool  audio_call_begin(void);    // starts the microphone and playback
void  audio_call_end(void);
void  audio_update(void);        // call every frame: reads the mic and sends audio
void  audio_play_frame(const unsigned char *frame, size_t len);
float audio_mic_level(void);     // 0..1, for the level meter
// voice-message recording (16 kHz ADPCM frames, max 2 minutes)
bool  audio_record_start(void);  // false while in a call or if the mic is unavailable
bool  audio_recording(void);     // false again once stopped or the limit is reached
void  audio_record_stop(void);
void  audio_record_discard(void);
u32   audio_record_ms(void);
const unsigned char *audio_record_data(size_t *len);
// voice-message playback; takes ownership of data (malloc'd ADPCM frames)
bool  audio_clip_play(unsigned char *data, size_t len);
void  audio_clip_stop(void);
bool  audio_clip_playing(void);
void  audio_clip_progress(u32 *pos_ms, u32 *total_ms);

// ---------------------------------------------------------------- camera.c
bool  cam_open(bool front, char *err, size_t errlen);
void  cam_close(void);
void  cam_update(void);          // call every frame while the camera screen is open
void  cam_switch(void);          // front <-> rear
bool  cam_is_front(void);
bool  cam_has_frame(void);
bool  cam_frozen(void);
bool  cam_snap(void);            // freeze the current frame
void  cam_retake(void);
void  cam_draw(float x, float y); // 320x240 preview
bool  cam_jpeg(unsigned char **out, unsigned long *outlen, int quality);

// ---------------------------------------------------------------- adpcm.c
typedef struct { int pred, index; } AdpcmState;
size_t adpcm_encode(AdpcmState *st, const s16 *in, int n, unsigned char *out);
int    adpcm_decode(const unsigned char *frame, size_t len, s16 *out, int max);

// ---------------------------------------------------------------- util
void  fmt_time(s64 ts, char *out, size_t n, bool with_date);
void  strlcpy_utf8(char *dst, const char *src, size_t n);
