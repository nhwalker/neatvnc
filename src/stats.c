/*
 * Copyright (c) 2026 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "stats.h"
#include "common.h"
#include "neatvnc.h"
#include "bandwidth.h"
#include "logging.h"
#include "enc/encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <aml.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#define STATS_INTERVAL_MS 500
#define STATS_SCHEMA_VERSION 1

static uint64_t stats__now_ms(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Returns the length of the valid UTF-8 sequence starting at p, or 0 if the
 * bytes at p are not one. Overlong encodings, surrogates and values beyond
 * U+10FFFF all count as invalid. Never reads past a NUL, because a NUL can
 * never pass the continuation-byte check.
 */
static int stats__utf8_sequence_length(const unsigned char* p)
{
	uint32_t cp;
	int len;

	if (p[0] < 0x80)
		return 1;
	else if ((p[0] & 0xe0) == 0xc0) { len = 2; cp = p[0] & 0x1f; }
	else if ((p[0] & 0xf0) == 0xe0) { len = 3; cp = p[0] & 0x0f; }
	else if ((p[0] & 0xf8) == 0xf0) { len = 4; cp = p[0] & 0x07; }
	else
		return 0;

	for (int i = 1; i < len; ++i) {
		if ((p[i] & 0xc0) != 0x80)
			return 0;
		cp = (cp << 6) | (p[i] & 0x3f);
	}

	if (len == 2 && cp < 0x80)
		return 0;
	if (len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
		return 0;
	if (len == 4 && (cp < 0x10000 || cp > 0x10ffff))
		return 0;

	return len;
}

/* JSON string escaping, for the handful of fields that come from outside:
 * peer addresses and authenticated user names.
 *
 * Bytes that do not form valid UTF-8 become U+FFFD. A user name is a value
 * the remote client chose, and RFC 8259 requires JSON to be UTF-8 -- passing
 * raw bytes through would let one client hand every non-browser consumer of
 * this file (python, jq) a decode error. Browsers already substitute U+FFFD
 * on decode; this makes every reader see the same thing.
 */
static void stats__put_json_string(FILE* out, const char* value)
{
	fputc('"', out);
	for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
		switch (*p) {
		case '"':  fputs("\\\"", out); break;
		case '\\': fputs("\\\\", out); break;
		case '\b': fputs("\\b", out); break;
		case '\f': fputs("\\f", out); break;
		case '\n': fputs("\\n", out); break;
		case '\r': fputs("\\r", out); break;
		case '\t': fputs("\\t", out); break;
		default:
			if (*p < 0x20) {
				fprintf(out, "\\u%04x", *p);
			} else if (*p < 0x80) {
				fputc(*p, out);
			} else {
				int len = stats__utf8_sequence_length(p);
				if (len > 0) {
					fwrite(p, 1, len, out);
					p += len - 1;
				} else {
					fputs("\\ufffd", out);
				}
			}
		}
	}
	fputc('"', out);
}

static void stats__format_address(const struct nvnc_client* client, char* dst,
		size_t size)
{
	struct sockaddr_storage addr = { 0 };
	socklen_t addrlen = sizeof(addr);

	if (nvnc_client_get_address(client, (struct sockaddr*)&addr,
				&addrlen) < 0) {
		snprintf(dst, size, "unknown");
		return;
	}

	char host[INET6_ADDRSTRLEN] = { 0 };

	switch (addr.ss_family) {
	case AF_INET: {
		const struct sockaddr_in* in = (const struct sockaddr_in*)&addr;
		inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
		snprintf(dst, size, "%s:%d", host, ntohs(in->sin_port));
		return;
	}
	case AF_INET6: {
		const struct sockaddr_in6* in6 =
			(const struct sockaddr_in6*)&addr;
		inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
		snprintf(dst, size, "[%s]:%d", host, ntohs(in6->sin6_port));
		return;
	}
	case AF_UNIX:
		snprintf(dst, size, "unix");
		return;
	default:
		snprintf(dst, size, "unknown");
		return;
	}
}

static const char* stats__encoding_name(const struct nvnc_client* client)
{
	if (!client->encoder)
		return "none";

	switch (encoder_get_type(client->encoder)) {
	case RFB_ENCODING_RAW: return "raw";
	case RFB_ENCODING_ZRLE: return "zrle";
	case RFB_ENCODING_TIGHT: return "tight";
	case RFB_ENCODING_OPEN_H264: return "open-h264";
	default: return "other";
	}
}

static void stats__write_client(FILE* out, struct nvnc_client* client,
		double interval_s)
{
	char address[128];
	stats__format_address(client, address, sizeof(address));

	uint64_t encoded = client->stats.frames_encoded;
	uint64_t dropped = client->stats.frames_dropped;
	uint64_t d_encoded = encoded - client->stats.prev_frames_encoded;
	uint64_t d_dropped = dropped - client->stats.prev_frames_dropped;

	client->stats.prev_frames_encoded = encoded;
	client->stats.prev_frames_dropped = dropped;

	uint64_t offered = d_encoded + d_dropped;

	/* Over the last interval only. A session-long average would stop
	 * responding to a change in conditions within a minute or two, which is
	 * the opposite of what a live indicator needs.
	 */
	double encoded_fps = interval_s > 0 ? d_encoded / interval_s : 0.0;
	double dropped_fps = interval_s > 0 ? d_dropped / interval_s : 0.0;
	double skip_fraction = offered > 0 ?
		(double)d_dropped / (double)offered : 0.0;

	const char* username = nvnc_client_get_auth_username(client);

	fprintf(out, "    {\n");
	fprintf(out, "      \"id\": %u,\n", client->stats.id);
	fprintf(out, "      \"address\": ");
	stats__put_json_string(out, address);
	fprintf(out, ",\n");
	fprintf(out, "      \"username\": ");
	if (username)
		stats__put_json_string(out, username);
	else
		fprintf(out, "null");
	fprintf(out, ",\n");
	fprintf(out, "      \"encoding\": ");
	stats__put_json_string(out, stats__encoding_name(client));
	fprintf(out, ",\n");
	fprintf(out, "      \"quality\": %d,\n", client->quality);
	fprintf(out, "      \"frames_encoded\": %" PRIu64 ",\n", encoded);
	fprintf(out, "      \"frames_dropped\": %" PRIu64 ",\n", dropped);
	fprintf(out, "      \"encoded_fps\": %.2f,\n", encoded_fps);
	fprintf(out, "      \"dropped_fps\": %.2f,\n", dropped_fps);
	fprintf(out, "      \"skip_fraction\": %.4f,\n", skip_fraction);
	fprintf(out, "      \"bandwidth_bps\": %.0f,\n",
			bwe_get_estimate(client->bwe) * 8.0);
	fprintf(out, "      \"min_rtt_us\": %d,\n", client->min_rtt);
	fprintf(out, "      \"inflight_bytes\": %d\n", client->inflight_bytes);
	fprintf(out, "    }");
}

static void stats__write(struct nvnc* server)
{
	const char* path = server->stats.path;

	/* Written beside the target and renamed, so a reader never sees a
	 * half-written file. Both must be on the same filesystem for rename to
	 * be atomic, hence the same directory rather than /tmp.
	 */
	char tmp_path[PATH_MAX];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		nvnc_log(NVNC_LOG_ERROR, "Stats path is too long: %s", path);
		return;
	}

	/* The temporary file's name is predictable, so it must never follow
	 * anything already sitting at that path -- in a directory writable by
	 * another local user, a planted symlink would otherwise redirect this
	 * write into an arbitrary file with the compositor's privileges. Drop
	 * whatever is there (a stale file from a crash, or the plant), then
	 * O_EXCL guarantees the file being written is one this call created.
	 * 0600 because nothing but the web server ever needs to read it, and
	 * the operator can relax the directory instead.
	 */
	unlink(tmp_path);
	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Could not create %s: %s", tmp_path,
				strerror(errno));
		return;
	}

	FILE* out = fdopen(fd, "w");
	if (!out) {
		nvnc_log(NVNC_LOG_ERROR, "Could not open %s: %s", tmp_path,
				strerror(errno));
		close(fd);
		unlink(tmp_path);
		return;
	}

	uint64_t now = stats__now_ms();
	double interval_s = server->stats.last_write_ms ?
		(now - server->stats.last_write_ms) / 1000.0 :
		STATS_INTERVAL_MS / 1000.0;
	server->stats.last_write_ms = now;

	fprintf(out, "{\n");
	fprintf(out, "  \"version\": %d,\n", STATS_SCHEMA_VERSION);
	fprintf(out, "  \"timestamp_ms\": %" PRIu64 ",\n", now);
	fprintf(out, "  \"interval_ms\": %d,\n", STATS_INTERVAL_MS);

	/* The rate the compositor is producing at, which is what the per-client
	 * encoded rate has to be read against.
	 */
	uint64_t offered = server->stats.frames_offered;
	uint64_t d_offered = offered - server->stats.prev_frames_offered;
	server->stats.prev_frames_offered = offered;
	fprintf(out, "  \"source_fps\": %.2f,\n",
			interval_s > 0 ? d_offered / interval_s : 0.0);
	fprintf(out, "  \"clients\": [\n");

	int count = 0;
	for (struct nvnc_client* c = nvnc_client_first(server); c;
			c = nvnc_client_next(c)) {
		if (count++)
			fprintf(out, ",\n");
		stats__write_client(out, c, interval_s);
	}

	fprintf(out, "\n  ]\n}\n");

	if (fclose(out) != 0) {
		nvnc_log(NVNC_LOG_ERROR, "Could not write %s: %s", tmp_path,
				strerror(errno));
		unlink(tmp_path);
		return;
	}

	if (rename(tmp_path, path) != 0) {
		nvnc_log(NVNC_LOG_ERROR, "Could not rename %s to %s: %s",
				tmp_path, path, strerror(errno));
		unlink(tmp_path);
	}
}

static void stats__on_tick(void* handle)
{
	stats__write(aml_get_userdata(handle));
}

void nvnc__stats_start(struct nvnc* server)
{
	const char* path = getenv("NVNC_STATS_FILE");
	if (!path || !path[0])
		return;

	server->stats.path = strdup(path);
	if (!server->stats.path)
		return;

	server->stats.ticker = aml_ticker_new(STATS_INTERVAL_MS * 1000,
			stats__on_tick, server, NULL);
	if (!server->stats.ticker) {
		free(server->stats.path);
		server->stats.path = NULL;
		return;
	}

	if (aml_start(aml_get_default(), server->stats.ticker) < 0) {
		aml_unref(server->stats.ticker);
		server->stats.ticker = NULL;
		free(server->stats.path);
		server->stats.path = NULL;
		return;
	}

	nvnc_log(NVNC_LOG_INFO, "Writing client stats to %s every %d ms", path,
			STATS_INTERVAL_MS);

	// So that a reader finds a valid file immediately rather than after the
	// first interval.
	stats__write(server);
}

void nvnc__stats_stop(struct nvnc* server)
{
	if (!server->stats.ticker)
		return;

	aml_stop(aml_get_default(), server->stats.ticker);
	aml_unref(server->stats.ticker);
	server->stats.ticker = NULL;

	unlink(server->stats.path);
	free(server->stats.path);
	server->stats.path = NULL;
}
