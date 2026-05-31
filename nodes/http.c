/* nodes/http.c -- http_in source node (fire-and-forget).
 *
 * Config:
 *   <name> http_in <bind> <port> [path]
 *
 * Spawns a single listener thread on <bind>:<port>. For every accepted
 * HTTP/1.0 or HTTP/1.1 request whose target matches [path] (default "/"),
 * the request body is pushed onto an internal inbox PQueue and the
 * client is immediately replied with `204 No Content`.
 *
 * This is intentionally fire-and-forget: there is no response
 * correlation, no streaming, no keep-alive, no TLS. Bodies larger than
 * MAX_BODY are rejected with 413. Slow clients are hard-timed out via
 * SO_RCVTIMEO/SO_SNDTIMEO so a hostile peer cannot pin the listener.
 *
 * If the inbox fills, the listener thread blocks on pqueue_push --
 * applying natural backpressure all the way to TCP accept().
 *
 * Bind to 127.0.0.1 if you only want loopback access; bind to 0.0.0.0
 * to accept from any interface. We do not invent defaults: the address
 * must be explicit in the config.
 */

#define _POSIX_C_SOURCE 200809L

#include "nodes.h"
#include "../http_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define INBOX_CAP   1024            /* power of two */
#define MAX_HEADER  4096
#define MAX_BODY    (1u << 20)      /* 1 MiB hard cap */
#define IO_TIMEOUT  5               /* seconds per recv/send */

typedef struct {
    char           bind[64];
    int            port;
    char           path[128];
    int            lfd;
    pthread_t      thr;
    int            has_thread;
    atomic_int     stop;            /* set in ctx_free */
    PQueue        *inbox;
    Node          *self;            /* for stderr prefixes */
} Ctx;

/* ---------- helpers ---------- */

/* Case-insensitive ASCII compare. */
static int ci_starts(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Find Content-Length value in headers. Returns -1 if absent, -2 if malformed. */
static long parse_content_length(const char *hdr, int hdrlen) {
    const char *p = hdr;
    const char *end = hdr + hdrlen;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        if (ci_starts(p, "content-length:")) {
            p += sizeof("content-length:") - 1;
            while (p < eol && (*p == ' ' || *p == '\t')) p++;
            char *endp = NULL;
            long v = strtol(p, &endp, 10);
            if (endp == p || v < 0) return -2;
            return v;
        }
        p = eol + 1;
    }
    return -1;
}

/* Returns 1 if the request-line target matches our configured path. */
static int target_matches(const char *req, int reqlen, const char *want) {
    /* Request line: METHOD SP TARGET SP VERSION CRLF */
    const char *sp1 = memchr(req, ' ', (size_t)reqlen);
    if (!sp1) return 0;
    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(reqlen - (target - req)));
    if (!sp2) return 0;
    /* Strip ?query for matching. */
    const char *q = memchr(target, '?', (size_t)(sp2 - target));
    size_t tlen = (size_t)((q ? q : sp2) - target);
    size_t wlen = strlen(want);
    return tlen == wlen && memcmp(target, want, tlen) == 0;
}

/* ---------- per-connection handler ---------- */

static void handle(int cfd, void *user) {
    Ctx *c = user;
    http_io_set_timeouts(cfd, IO_TIMEOUT);

    char hbuf[MAX_HEADER];
    int  total = 0;
    int  hlen = http_io_read_headers(cfd, hbuf, sizeof(hbuf), &total);
    if (hlen == -2) { http_io_send_status(cfd, "431 Request Header Fields Too Large"); return; }
    if (hlen <  0)  { return; }

    if (!target_matches(hbuf, hlen, c->path)) {
        http_io_send_status(cfd, "404 Not Found");
        return;
    }

    long clen = parse_content_length(hbuf, hlen);
    if (clen == -2) { http_io_send_status(cfd, "400 Bad Request"); return; }
    if (clen < 0)   clen = 0;                  /* GET / no body */
    if ((unsigned long)clen > MAX_BODY) {
        http_io_send_status(cfd, "413 Payload Too Large");
        return;
    }

    /* Bytes of body already read while parsing headers. */
    int already = total - hlen;
    if (already < 0) already = 0;

    Packet *p = packet_new(NULL, (size_t)clen);
    if (!p) { http_io_send_status(cfd, "500 Internal Server Error"); return; }

    if (already > clen) already = (int)clen;
    if (already > 0) memcpy(p->data, hbuf + hlen, (size_t)already);

    size_t got = (size_t)already;
    while (got < (size_t)clen) {
        ssize_t r = recv(cfd, (char *)p->data + got, (size_t)clen - got, 0);
        if (r < 0) { if (errno == EINTR) continue; packet_release(p); return; }
        if (r == 0) { packet_release(p); return; }
        got += (size_t)r;
    }

    /* Push to inbox (blocks under backpressure). Close == shutdown -> drop. */
    if (pqueue_push(c->inbox, p) != 0) {
        packet_release(p);
        http_io_send_status(cfd, "503 Service Unavailable");
        return;
    }
    http_io_send_status(cfd, "204 No Content");
}

/* ---------- listener thread ---------- */

static void *listener_main(void *arg) {
    Ctx *c = arg;
    http_io_serve(c->lfd, &c->stop, handle, c);
    return NULL;
}

/* ---------- source process: pop from inbox ---------- */

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx; (void)in;
    Ctx *c = self->ctx;
    /* Blocks on the inbox condvar; on_stop closes it at shutdown,
     * which wakes us with pop returning NULL. */
    return pqueue_pop(c->inbox);
}

static void on_stop(Node *self) {
    Ctx *c = self->ctx;
    /* Stop the listener thread and wake any pqueue_pop on the inbox. */
    c->stop = 1;
    if (c->lfd >= 0)  shutdown(c->lfd, SHUT_RDWR);
    if (c->inbox)     pqueue_close(c->inbox);
}

/* ---------- teardown ---------- */

static void ctx_free(void *vp) {
    Ctx *c = vp;
    if (c->has_thread) {
        c->stop = 1;
        if (c->lfd >= 0) shutdown(c->lfd, SHUT_RDWR);  /* unblock accept */
        if (c->inbox)    pqueue_close(c->inbox);       /* unblock push */
        pthread_join(c->thr, NULL);
    }
    if (c->lfd >= 0) close(c->lfd);
    if (c->inbox) {
        Packet *p;
        while (pqueue_trypop(c->inbox, &p) == 0) packet_release(p);
        pqueue_free(c->inbox);
    }
    free(c);
}

/* ---------- init ---------- */

static int parse_args(const char *args, Ctx *c) {
    if (!args || !*args) return -1;
    char path[128] = "/";
    int  scanned = sscanf(args, "%63s %d %127s", c->bind, &c->port, path);
    if (scanned < 2) return -1;
    if (c->port < 1 || c->port > 65535) return -1;
    if (path[0] != '/') return -1;
    memcpy(c->path, path, sizeof(c->path));
    return 0;
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->lfd = -1;
    c->self = n;

    if (parse_args(args, c) != 0) {
        fprintf(stderr, "jerboa: http_in expects: <bind> <port> [path]\n");
        free(c); return -1;
    }

    /* Resolve and bind. Strict numeric address (no DNS); we only ever bind
     * an interface, never connect, so a hostname would be misleading. */
    struct sockaddr_in sa = { .sin_family = AF_INET };
    sa.sin_port = htons((uint16_t)c->port);
    if (inet_pton(AF_INET, c->bind, &sa.sin_addr) != 1) {
        fprintf(stderr, "jerboa: http_in: bad bind address '%s' (use a dotted IPv4)\n",
                c->bind);
        free(c); return -1;
    }

    c->lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->lfd < 0) { perror("http_in: socket"); free(c); return -1; }
    int one = 1;
    setsockopt(c->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(c->lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "jerboa: http_in: bind %s:%d: %s\n",
                c->bind, c->port, strerror(errno));
        close(c->lfd); free(c); return -1;
    }
    if (listen(c->lfd, 64) < 0) {
        perror("http_in: listen");
        close(c->lfd); free(c); return -1;
    }

    c->inbox = pqueue_new(INBOX_CAP);
    if (!c->inbox) { close(c->lfd); free(c); return -1; }

    if (pthread_create(&c->thr, NULL, listener_main, c) != 0) {
        pqueue_free(c->inbox); close(c->lfd); free(c); return -1;
    }
    c->has_thread = 1;

    n->ctx             = c;
    n->ctx_free        = ctx_free;
    n->process         = process;
    n->on_stop         = on_stop;
    n->src_interval_ms = 0;     /* we block inside process() ourselves */
    return 0;
}

const NodeType ndt_http_in = { "http_in", init };
