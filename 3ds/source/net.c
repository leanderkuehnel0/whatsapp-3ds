// TLS connection to the bridge and line framing (see PROTOCOL.md).
// The certificate is not validated against a CA; instead its SHA-256 is pinned by the caller.
#include "app.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

static u32 *soc_buf;
static int sock = -1;
static bool tls_ready;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_config conf;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;

// receive buffer for line framing
static char  *rx;
static size_t rx_len, rx_cap;
// pending binary payload
static unsigned char *bin;
static size_t bin_len, bin_got;
static char   bin_tag[16];
static char   bin_kind[8];
static int    bin_w, bin_h;

void net_init(void)
{
	soc_buf = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (soc_buf)
		socInit(soc_buf, SOC_BUFFERSIZE);
	psInit();
}

void net_exit(void)
{
	net_close();
	psExit();
	socExit();
	free(soc_buf);
}

static int entropy_source(void *data, unsigned char *out, size_t len, size_t *olen)
{
	(void)data;
	if (R_FAILED(PS_GenerateRandomBytes(out, len)))
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	*olen = len;
	return 0;
}

static int last_sock_errno;

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	(void)ctx;
	int r = send(sock, buf, len, 0);
	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		last_sock_errno = errno;
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	(void)ctx;
	int r = recv(sock, buf, len, 0);
	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_READ;
		last_sock_errno = errno;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	if (r == 0)
		last_sock_errno = ECONNRESET;
	return r; // 0 = peer closed
}

static void sha256(const unsigned char *in, size_t len, unsigned char out[32])
{
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
	mbedtls_sha256(in, len, out, 0);
#else
	mbedtls_sha256_ret(in, len, out, 0);
#endif
}

bool net_connected(void) { return tls_ready; }

// ---------------------------------------------------------------- connecting
// Connecting runs on a worker thread so the UI stays responsive (and SELECT works)
// while a bridge is unreachable. All TLS state belongs to the worker until it finishes.

#define CONNECT_TIMEOUT_MS   8000
#define HANDSHAKE_TIMEOUT_MS 20000

static Thread        conn_thread;
static volatile bool conn_cancel;
static volatile int  conn_result; // 0 running, 1 ok, -1 failed
static char          conn_host[128];
static int           conn_port;
static unsigned char conn_fp[32];
static char          conn_err[128];

static void tx_free(void);

static void tls_teardown(void)
{
	if (tls_ready) {
		mbedtls_ssl_close_notify(&ssl);
		tls_ready = false;
	}
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	if (sock >= 0) {
		close(sock);
		sock = -1;
	}
	free(rx);
	rx = NULL;
	rx_len = rx_cap = 0;
	tx_free();
	free(bin);
	bin = NULL;
	bin_len = bin_got = 0;
}

static void conn_stop(void)
{
	if (!conn_thread)
		return;
	conn_cancel = true;
	threadJoin(conn_thread, U64_MAX);
	threadFree(conn_thread);
	conn_thread = NULL;
}

void net_close(void)
{
	conn_stop();
	tls_teardown();
}

static bool fail(char *err, size_t errlen, const char *what, int code)
{
	if (code)
		snprintf(err, errlen, "%s (%d)", what, code);
	else
		snprintf(err, errlen, "%s", what);
	tls_teardown();
	return false;
}

// Waits until the socket is readable/writable. Returns 1 ready, 0 timed out, -1 cancelled.
static int wait_sock(short events, u64 deadline)
{
	while (!conn_cancel) {
		if (osGetTime() >= deadline)
			return 0;
		struct pollfd pfd = {.fd = sock, .events = events};
		int r = poll(&pfd, 1, 200);
		if (r > 0)
			return 1;
		if (r < 0 && errno != EINTR)
			return 1; // let the next socket call report the actual error
	}
	return -1;
}

static bool connect_blocking(const char *host, int port, unsigned char fp[32], char *err, size_t errlen)
{
	tls_teardown();
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_entropy_init(&entropy);

	if (!soc_buf)
		return fail(err, errlen, "Network init failed", 0);

	char portstr[8];
	snprintf(portstr, sizeof portstr, "%d", port);
	struct addrinfo hints = {0}, *res = NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	int r = getaddrinfo(host, portstr, &hints, &res);
	if (r != 0 || !res)
		return fail(err, errlen, "Could not find the bridge address", r);
	if (conn_cancel) {
		freeaddrinfo(res);
		return fail(err, errlen, "Cancelled", 0);
	}

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		freeaddrinfo(res);
		return fail(err, errlen, "socket() failed", errno);
	}
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
	r = connect(sock, res->ai_addr, res->ai_addrlen);
	int connect_errno = errno;
	freeaddrinfo(res);
	if (r < 0 && connect_errno != EINPROGRESS && connect_errno != EALREADY && connect_errno != EWOULDBLOCK) {
		char msg[96];
		snprintf(msg, sizeof msg, "connect() failed: %s", strerror(connect_errno));
		return fail(err, errlen, msg, connect_errno);
	}
	if (r < 0) {
		int w = wait_sock(POLLOUT, osGetTime() + CONNECT_TIMEOUT_MS);
		if (w < 0)
			return fail(err, errlen, "Cancelled", 0);
		if (w == 0)
			return fail(err, errlen, "The bridge is not reachable (timed out).\nIs it running, and is the address right?", 0);
		// Deliberately no getsockopt(SO_ERROR) here: on the 3DS it passes the SOC service's raw
		// value through and can report an error for a connection that worked. If the connect
		// really failed, the TLS handshake below fails on its first send/receive instead.
	}

	mbedtls_entropy_add_source(&entropy, entropy_source, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
	const char *pers = "wa3ds";
	if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers))) != 0)
		return fail(err, errlen, "RNG seed failed", r);
	if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		return fail(err, errlen, "TLS config failed", r);

	// No CA: the certificate is pinned by fingerprint after the handshake instead.
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
#if MBEDTLS_VERSION_NUMBER >= 0x03020000
	mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
#endif
	if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0)
		return fail(err, errlen, "TLS setup failed", r);
	mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);

	u64 deadline = osGetTime() + HANDSHAKE_TIMEOUT_MS;
	while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (r == MBEDTLS_ERR_NET_SEND_FAILED || r == MBEDTLS_ERR_NET_RECV_FAILED || r == MBEDTLS_ERR_NET_CONN_RESET) {
			char msg[96];
			snprintf(msg, sizeof msg, "Connection to the bridge failed: %s", strerror(last_sock_errno));
			return fail(err, errlen, msg, last_sock_errno);
		}
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
			return fail(err, errlen, "TLS handshake failed - is this really the bridge?", r);
		int w = wait_sock(r == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, deadline);
		if (w < 0)
			return fail(err, errlen, "Cancelled", 0);
		if (w == 0)
			return fail(err, errlen, "TLS handshake timed out", 0);
	}

	const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
	if (!peer)
		return fail(err, errlen, "Bridge sent no certificate", 0);
	sha256(peer->raw.p, peer->raw.len, fp);
	return true;
}

static void conn_worker(void *arg)
{
	(void)arg;
	bool ok = connect_blocking(conn_host, conn_port, conn_fp, conn_err, sizeof conn_err);
	conn_result = ok ? 1 : -1;
}

bool net_connect_begin(const char *host, int port)
{
	net_close();
	strlcpy_utf8(conn_host, host, sizeof conn_host);
	conn_port = port;
	conn_cancel = false;
	conn_result = 0;
	conn_err[0] = 0;

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// lower priority than the UI; big stack for the TLS handshake
	conn_thread = threadCreate(conn_worker, NULL, 128 * 1024, prio + 1 > 0x3F ? 0x3F : prio + 1, -2, false);
	if (!conn_thread) {
		snprintf(conn_err, sizeof conn_err, "Could not start the network thread");
		conn_result = -1;
		return false;
	}
	return true;
}

// true from net_connect_begin until net_connect_poll has collected the result
bool net_connecting(void) { return conn_thread != NULL; }

int net_connect_poll(unsigned char fp[32], char *err, size_t errlen)
{
	if (conn_result == 0)
		return 0;
	if (conn_thread) {
		threadJoin(conn_thread, U64_MAX);
		threadFree(conn_thread);
		conn_thread = NULL;
	}
	int r = conn_result;
	conn_result = 0;
	if (r > 0) {
		memcpy(fp, conn_fp, 32);
		tls_ready = true;
		return 1;
	}
	snprintf(err, errlen, "%s", conn_err[0] ? conn_err : "Connection failed");
	return -1;
}

// ---------------------------------------------------------------- framing

static void append_escaped(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t need = *len + strlen(s) * 2 + 4;
	if (need > *cap) {
		*cap = need * 2;
		*buf = realloc(*buf, *cap);
	}
	for (; *s; s++) {
		char c = *s;
		const char *esc = NULL;
		switch (c) {
		case '\\': esc = "\\\\"; break;
		case '\t': esc = "\\t"; break;
		case '\n': esc = "\\n"; break;
		case '\r': esc = "\\r"; break;
		}
		if (esc) {
			(*buf)[(*len)++] = esc[0];
			(*buf)[(*len)++] = esc[1];
		} else {
			(*buf)[(*len)++] = c;
		}
	}
}

// ---------------------------------------------------------------- send queue
// Every write goes through one ordered queue that is drained without blocking (here and in
// net_poll), so large uploads don't freeze the UI and messages never interleave.

static unsigned char *tx;
static size_t tx_len, tx_off, tx_cap;
static size_t tx_inflight;   // length passed to a write that returned WANT_*; must be repeated
static size_t tx_mark_end;   // end offset of the upload being tracked for progress
static size_t tx_mark_start;

static bool tx_append(const void *p, size_t n)
{
	if (tx_off > 0 && tx_off == tx_len) // fully sent: reuse the buffer from the start
		tx_off = tx_len = tx_mark_start = tx_mark_end = 0;
	if (tx_len + n > tx_cap) {
		size_t cap = tx_cap ? tx_cap : 16384;
		while (cap < tx_len + n)
			cap *= 2;
		unsigned char *b = realloc(tx, cap);
		if (!b)
			return false;
		tx = b;
		tx_cap = cap;
	}
	memcpy(tx + tx_len, p, n);
	tx_len += n;
	return true;
}

static bool tx_flush(void)
{
	while (tx_off < tx_len) {
		size_t n = tx_inflight ? tx_inflight : tx_len - tx_off;
		if (n > 16384)
			n = 16384;
		int r = mbedtls_ssl_write(&ssl, tx + tx_off, n);
		if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
			tx_inflight = n;
			return true; // socket full; continue next frame
		}
		tx_inflight = 0;
		if (r <= 0)
			return false;
		tx_off += r;
	}
	if (tx_off == tx_len) // everything sent: reset the buffer and the upload marks
		tx_off = tx_len = tx_mark_start = tx_mark_end = 0;
	return true;
}

static void tx_free(void)
{
	free(tx);
	tx = NULL;
	tx_len = tx_off = tx_cap = tx_inflight = tx_mark_start = tx_mark_end = 0;
}

static bool append_line(const char **fields, int nfields)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (int i = 0; i < nfields; i++) {
		append_escaped(&buf, &len, &cap, fields[i] ? fields[i] : "");
		buf[len++] = i + 1 < nfields ? '\t' : '\n';
	}
	bool ok = tx_append(buf, len);
	free(buf);
	return ok;
}

static bool flush_or_close(void)
{
	if (!tx_flush()) {
		net_close();
		return false;
	}
	return true;
}

bool net_send(const char **fields, int nfields)
{
	if (!tls_ready || !append_line(fields, nfields))
		return false;
	return flush_or_close();
}

bool net_send_payload(const char **fields, int nfields, const unsigned char *data, size_t len)
{
	if (!tls_ready)
		return false;
	tx_mark_start = tx_len;
	if (!append_line(fields, nfields) || !tx_append(data, len))
		return false;
	tx_mark_end = tx_len;
	return flush_or_close();
}

bool net_upload_progress(size_t *sent, size_t *total)
{
	if (!tx_mark_end || tx_mark_end <= tx_mark_start) {
		*sent = *total = 0;
		return false;
	}
	*total = tx_mark_end - tx_mark_start;
	*sent = tx_off > tx_mark_start ? tx_off - tx_mark_start : 0;
	if (*sent > *total)
		*sent = *total;
	return *sent < *total;
}

static void unescape(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			switch (*r) {
			case 't': *w++ = '\t'; break;
			case 'n': *w++ = '\n'; break;
			case 'r': *w++ = '\r'; break;
			case '\\': *w++ = '\\'; break;
			default: *w++ = '\\'; *w++ = *r; break;
			}
		} else {
			*w++ = *r;
		}
	}
	*w = 0;
}

#define MAX_FIELDS 16

static void dispatch_line(char *l, LineHandler on_line)
{
	char *f[MAX_FIELDS];
	int n = 0;
	char *p = l;
	f[n++] = p;
	while (*p && n < MAX_FIELDS) {
		if (*p == '\t') {
			*p = 0;
			f[n++] = p + 1;
		}
		p++;
	}
	for (int i = 0; i < n; i++)
		unescape(f[i]);

	if (n >= 3 && (strcmp(f[1], "BIN") == 0 || strcmp(f[1], "AUD") == 0)) {
		bin_len = strtoul(f[2], NULL, 10);
		bin_w = n > 3 ? atoi(f[3]) : 0;
		bin_h = n > 4 ? atoi(f[4]) : 0;
		strlcpy_utf8(bin_tag, f[0], sizeof bin_tag);
		strlcpy_utf8(bin_kind, f[1], sizeof bin_kind);
		bin_got = 0;
		bin = malloc(bin_len ? bin_len : 1);
		return;
	}
	on_line(f, n);
}

// Consumes bytes from rx: binary payload first if one is pending, then complete lines.
static void process_rx(LineHandler on_line, BinHandler on_bin)
{
	size_t off = 0;
	while (off < rx_len) {
		if (bin) {
			size_t take = rx_len - off;
			if (take > bin_len - bin_got)
				take = bin_len - bin_got;
			memcpy(bin + bin_got, rx + off, take);
			bin_got += take;
			off += take;
			if (bin_got == bin_len) {
				unsigned char *data = bin;
				bin = NULL;
				on_bin(bin_tag, bin_kind, bin_w, bin_h, data, bin_len);
				if (!tls_ready)
					return;
			}
			continue;
		}
		char *nl = memchr(rx + off, '\n', rx_len - off);
		if (!nl)
			break;
		*nl = 0;
		if (nl > rx + off && nl[-1] == '\r')
			nl[-1] = 0;
		dispatch_line(rx + off, on_line);
		off = (nl - rx) + 1;
		if (!tls_ready)
			return; // a handler closed the connection
	}
	memmove(rx, rx + off, rx_len - off);
	rx_len -= off;
}

bool net_poll(LineHandler on_line, BinHandler on_bin)
{
	if (!tls_ready)
		return false;
	if (!flush_or_close())
		return false;
	for (int rounds = 0; rounds < 64; rounds++) {
		if (rx_cap - rx_len < 4096) {
			rx_cap = rx_cap ? rx_cap * 2 : 16384;
			rx = realloc(rx, rx_cap);
		}
		int r = mbedtls_ssl_read(&ssl, (unsigned char *)rx + rx_len, rx_cap - rx_len - 1);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			break;
		if (r <= 0) {
			net_close();
			return false;
		}
		rx_len += r;
		process_rx(on_line, on_bin);
		if (!tls_ready)
			return false;
	}
	return true;
}

bool net_send_binary(const char *tag, const char *kind, const unsigned char *data, size_t len)
{
	if (!tls_ready)
		return false;
	// call audio is real-time: drop frames rather than queue behind a big upload
	if (tx_len - tx_off > 32 * 1024)
		return true;
	char head[64];
	int h = snprintf(head, sizeof head, "%s\t%s\t%u\n", tag, kind, (unsigned)len);
	if (!tx_append(head, h) || !tx_append(data, len))
		return false;
	return flush_or_close();
}

size_t net_bin_progress(size_t *total)
{
	bool img = bin && strcmp(bin_kind, "BIN") == 0;
	if (total)
		*total = img ? bin_len : 0;
	return img ? bin_got : 0;
}
