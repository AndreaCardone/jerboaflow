/* http_io.h -- internal HTTP/1.0 plumbing shared by http_in, http_out
 * and the metrics endpoint. NOT a general library: just the bits the
 * three call sites would otherwise duplicate. Request parsing policy
 * (target matching, body framing, response rendering) stays in the
 * caller. */
#ifndef JERBOA_HTTP_IO_H
#define JERBOA_HTTP_IO_H

#include <stddef.h>
#include <stdatomic.h>

/* Write n bytes, retrying on EINTR. Uses MSG_NOSIGNAL so a peer that
 * vanishes does not raise SIGPIPE. Returns 0 on success, -1 on error. */
int  http_io_write_all(int fd, const void *buf, size_t n);

/* SO_RCVTIMEO + SO_SNDTIMEO in whole seconds. A slow or hostile peer
 * cannot pin a worker beyond this. */
void http_io_set_timeouts(int fd, int seconds);

/* Send "HTTP/1.0 <status>\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".
 * `status` is the full reason-phrase including code, e.g. "404 Not Found".
 * Best-effort: write errors are ignored (we're about to close anyway). */
void http_io_send_status(int fd, const char *status);

/* Read into buf until \r\n\r\n is seen or cap is exhausted. On success,
 * returns header length including the trailing CRLFCRLF; *total receives
 * the total bytes in buf (header + any pipelined body bytes already
 * consumed). Returns -1 on I/O error or EOF, -2 if cap was exhausted
 * before end-of-headers. */
int  http_io_read_headers(int fd, char *buf, int cap, int *total);

/* Listener skeleton. Caller owns lfd (already bound + listening) and
 * closes it after this returns. Runs accept/handle/close in a loop on
 * the caller's thread, polling *stop every ~200 ms so shutdown latency
 * is bounded. The handler is invoked synchronously; the accepted fd is
 * closed by the skeleton after the handler returns. */
typedef void (*http_io_handler)(int cfd, void *user);
void http_io_serve(int lfd, atomic_int *stop,
                   http_io_handler handle, void *user);

/* ---------- HTTP/1.0 client ---------- */

/* Parsed http:// URL. Fixed-size, no allocation. */
typedef struct {
    char host[256];
    char port[8];           /* numeric service, fits "65535" */
    char path[512];         /* always starts with '/' */
} HttpUrl;

/* Result of one client round-trip. `body` borrows into `raw`; callers
 * must release with http_response_free() (never free(r->body)). */
typedef struct {
    int    status;          /* HTTP status code, -1 if response malformed */
    char  *body;            /* points into raw; NULL if absent */
    size_t body_len;        /* unclamped length seen; may exceed any caller cap */
    int    truncated;       /* 1 if peer sent more than raw_cap could hold */
    char  *raw;             /* owned malloc; opaque to caller */
    size_t raw_len;
} HttpResponse;

/* Parse "http://host[:port]/path" into out. Writes a one-line diagnostic
 * to stderr prefixed by `who` on failure. Returns 0 on success, -1 otherwise. */
int  http_url_parse(const char *url, HttpUrl *out, const char *who);

/* One blocking HTTP/1.0 round-trip. Performs DNS, connect, send request
 * line + headers + body, shutdown(SHUT_WR), drain response up to raw_cap
 * bytes, parse status line, locate body.
 *
 * Returns 0 on a completed round-trip (any HTTP status, including 5xx);
 * the caller MUST then call http_response_free(out). Returns -1 on
 * network/DNS/protocol failure with a stderr diagnostic prefixed by
 * `who`; out is left untouched. */
int  http_client_do(const HttpUrl *u,
                    const char *method, const char *ctype,
                    const void *body, size_t body_len,
                    size_t raw_cap, int timeout_s,
                    const char *who,
                    HttpResponse *out);

void http_response_free(HttpResponse *r);

#endif
