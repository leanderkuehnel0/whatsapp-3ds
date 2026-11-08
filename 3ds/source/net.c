// TLS connection to the bridge and line framing (see PROTOCOL.md).
// The certificate is not validated against a CA; instead its SHA-256 is pinned by the caller.
#include "app.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
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

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	(void)ctx;
	int r = send(sock, buf, len, 0);
	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
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
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
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

void net_close(void)
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
	free(bin);
	bin = NULL;
	bin_len = bin_got = 0;
}

static bool fail(char *err, size_t errlen, const char *what, int code)
{
	snprintf(err, errlen, "%s (%d)", what, code);
	net_close();
	return false;
}

bool net_connect(const char *host, int port, unsigned char fp[32], char *err, size_t errlen)
{
	net_close();
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
		return fail(err, errlen, "Could not resolve host", r);

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		freeaddrinfo(res);
		return fail(err, errlen, "socket() failed", errno);
	}
	r = connect(sock, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (r < 0)
		return fail(err, errlen, "Could not connect to bridge", errno);

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

	while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
			return fail(err, errlen, "TLS handshake failed", r);
	}

	const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
	if (!peer)
		return fail(err, errlen, "Bridge sent no certificate", 0);
	sha256(peer->raw.p, peer->raw.len, fp);

	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
	tls_ready = true;
	return true;
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

static bool write_all(const unsigned char *p, size_t len)
{
	while (len > 0) {
		int r = mbedtls_ssl_write(&ssl, p, len);
		if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
			svcSleepThread(1000000); // 1 ms
			continue;
		}
		if (r <= 0)
			return false;
		p += r;
		len -= r;
	}
	return true;
}

bool net_send(const char **fields, int nfields)
{
	if (!tls_ready)
		return false;
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (int i = 0; i < nfields; i++) {
		append_escaped(&buf, &len, &cap, fields[i] ? fields[i] : "");
		buf[len++] = i + 1 < nfields ? '\t' : '\n';
	}
	bool ok = write_all((unsigned char *)buf, len);
	free(buf);
	if (!ok)
		net_close();
	return ok;
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

	if (n >= 3 && strcmp(f[1], "BIN") == 0) {
		bin_len = strtoul(f[2], NULL, 10);
		bin_w = n > 3 ? atoi(f[3]) : 0;
		bin_h = n > 4 ? atoi(f[4]) : 0;
		strlcpy_utf8(bin_tag, f[0], sizeof bin_tag);
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
				on_bin(bin_tag, bin_w, bin_h, data, bin_len);
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

size_t net_bin_progress(size_t *total)
{
	if (total)
		*total = bin ? bin_len : 0;
	return bin ? bin_got : 0;
}
