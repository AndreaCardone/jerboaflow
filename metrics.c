/* metrics.c -- tiny Prometheus /metrics HTTP endpoint.
 *
 * One listener thread, blocking accept (via epoll_wait with a 200ms
 * tick so we can notice the stop flag). One connection at a time --
 * scrapers are sequential and metrics rendering is microseconds. No
 * HTTP library: we read the request line, ignore headers, and write a
 * fixed response.
 *
 * Always-on counters live on Node and PQueue in the runtime; this file
 * only snapshots and formats them. */

#define _POSIX_C_SOURCE 200809L

#include "jerboa.h"
#include "http_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    Flow       *flow;
    int         lfd;
    int         port;
    pthread_t   thr;
    atomic_int  stop;
    int         running;
} Metrics;

static Metrics g_m;

/* ---------- growable buffer ---------- */

typedef struct { char *p; size_t n, cap; } Buf;

static int buf_reserve(Buf *b, size_t add) {
    /* Overflow-safe growth: cap at SIZE_MAX/2 to keep doubling sane. */
    if (add >= SIZE_MAX - b->n - 1) return -1;
    size_t need = b->n + add + 1;
    if (need <= b->cap) return 0;
    size_t c = b->cap ? b->cap : 4096;
    while (c < need) {
        if (c > SIZE_MAX / 2) { c = need; break; }
        c *= 2;
    }
    char *q = realloc(b->p, c);
    if (!q) return -1;
    b->p = q; b->cap = c;
    return 0;
}

static int buf_printf(Buf *b, const char *fmt, ...) {
    if (b->n == SIZE_MAX) return -1;       /* sticky error from previous call */
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || buf_reserve(b, (size_t)needed) < 0) {
        va_end(ap2);
        b->n = SIZE_MAX;
        return -1;
    }
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap2);
    va_end(ap2);
    b->n += (size_t)needed;
    return 0;
}

static int buf_had_error(const Buf *b) { return b->n == SIZE_MAX; }

/* ---------- render ---------- */

/* Sanitize a label value -- backslash, quote, newline. */
static void esc_label(char *dst, size_t dstlen, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstlen; i++) {
        char c = src[i];
        if (c == '\\' || c == '"') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
        else                       { dst[j++] = c; }
    }
    dst[j] = '\0';
}

static void render(Flow *f, Buf *b) {
    buf_printf(b,
        "# HELP jerboa_node_packets_in_total Packets consumed by node process().\n"
        "# TYPE jerboa_node_packets_in_total counter\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        char en[64], et[32];
        esc_label(en, sizeof(en), n->name);
        esc_label(et, sizeof(et), n->type);
        buf_printf(b, "jerboa_node_packets_in_total{node=\"%s\",type=\"%s\"} %llu\n",
                   en, et,
                   (unsigned long long)atomic_load_explicit(&n->m_pkts_in, memory_order_relaxed));
    }

    buf_printf(b,
        "# HELP jerboa_node_packets_out_total Packets emitted by node.\n"
        "# TYPE jerboa_node_packets_out_total counter\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        char en[64], et[32];
        esc_label(en, sizeof(en), n->name);
        esc_label(et, sizeof(et), n->type);
        buf_printf(b, "jerboa_node_packets_out_total{node=\"%s\",type=\"%s\"} %llu\n",
                   en, et,
                   (unsigned long long)atomic_load_explicit(&n->m_pkts_out, memory_order_relaxed));
    }

    buf_printf(b,
        "# HELP jerboa_node_process_calls_total Number of process() invocations.\n"
        "# TYPE jerboa_node_process_calls_total counter\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        char en[64], et[32];
        esc_label(en, sizeof(en), n->name);
        esc_label(et, sizeof(et), n->type);
        buf_printf(b, "jerboa_node_process_calls_total{node=\"%s\",type=\"%s\"} %llu\n",
                   en, et,
                   (unsigned long long)atomic_load_explicit(&n->m_process_calls, memory_order_relaxed));
    }

    buf_printf(b,
        "# HELP jerboa_node_process_seconds_total Total wallclock time inside process().\n"
        "# TYPE jerboa_node_process_seconds_total counter\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        char en[64], et[32];
        esc_label(en, sizeof(en), n->name);
        esc_label(et, sizeof(et), n->type);
        uint64_t ns = atomic_load_explicit(&n->m_process_ns, memory_order_relaxed);
        buf_printf(b, "jerboa_node_process_seconds_total{node=\"%s\",type=\"%s\"} %.9f\n",
                   en, et, (double)ns / 1e9);
    }

    /* Per-queue metrics keyed by consumer node + input port. */
    buf_printf(b,
        "# HELP jerboa_queue_depth Current packets in queue.\n"
        "# TYPE jerboa_queue_depth gauge\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        for (size_t p = 0; p < n->n_in; p++) {
            PQueue *q = n->in[p];
            if (!q) continue;
            pthread_mutex_lock(&q->mtx);
            size_t depth = q->count;
            pthread_mutex_unlock(&q->mtx);
            char en[64]; esc_label(en, sizeof(en), n->name);
            buf_printf(b, "jerboa_queue_depth{node=\"%s\",port=\"%zu\"} %zu\n",
                       en, p, depth);
        }
    }

    buf_printf(b,
        "# HELP jerboa_queue_high_water_packets Max queue depth observed.\n"
        "# TYPE jerboa_queue_high_water_packets gauge\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        for (size_t p = 0; p < n->n_in; p++) {
            PQueue *q = n->in[p];
            if (!q) continue;
            pthread_mutex_lock(&q->mtx);
            size_t hw = q->m_highwater;
            pthread_mutex_unlock(&q->mtx);
            char en[64]; esc_label(en, sizeof(en), n->name);
            buf_printf(b, "jerboa_queue_high_water_packets{node=\"%s\",port=\"%zu\"} %zu\n",
                       en, p, hw);
        }
    }

    buf_printf(b,
        "# HELP jerboa_queue_pushed_total Successful pushes.\n"
        "# TYPE jerboa_queue_pushed_total counter\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        for (size_t p = 0; p < n->n_in; p++) {
            PQueue *q = n->in[p];
            if (!q) continue;
            pthread_mutex_lock(&q->mtx);
            uint64_t v = q->m_pushed;
            pthread_mutex_unlock(&q->mtx);
            char en[64]; esc_label(en, sizeof(en), n->name);
            buf_printf(b, "jerboa_queue_pushed_total{node=\"%s\",port=\"%zu\"} %llu\n",
                       en, p, (unsigned long long)v);
        }
    }

    buf_printf(b,
        "# HELP jerboa_queue_dropped_total Pushes rejected because queue was closed.\n"
        "# TYPE jerboa_queue_dropped_total counter\n");
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        for (size_t p = 0; p < n->n_in; p++) {
            PQueue *q = n->in[p];
            if (!q) continue;
            pthread_mutex_lock(&q->mtx);
            uint64_t v = q->m_dropped;
            pthread_mutex_unlock(&q->mtx);
            char en[64]; esc_label(en, sizeof(en), n->name);
            buf_printf(b, "jerboa_queue_dropped_total{node=\"%s\",port=\"%zu\"} %llu\n",
                       en, p, (unsigned long long)v);
        }
    }
}

/* ---------- HTTP handling ---------- */

static void handle(int cfd, void *user) {
    Flow *f = user;
    http_io_set_timeouts(cfd, 5);

    char req[1024];
    ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    /* Require a CRLF in what we read -- if the client didn't send a full
     * request line in <1KB it's either malformed or hostile. */
    if (!memchr(req, '\n', (size_t)n)) {
        http_io_send_status(cfd, "400 Bad Request");
        return;
    }

    /* We only care about the request target on the first line. */
    if (strncmp(req, "GET /metrics", 12) != 0) {
        http_io_send_status(cfd, "404 Not Found");
        return;
    }

    Buf body = {0};
    render(f, &body);
    if (buf_had_error(&body)) {
        free(body.p);
        http_io_send_status(cfd, "500 Internal Server Error");
        return;
    }

    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", body.n);
    http_io_write_all(cfd, hdr, (size_t)hn);
    if (body.n) http_io_write_all(cfd, body.p, body.n);
    free(body.p);
}

/* ---------- listener thread ---------- */

static void *metrics_main(void *arg) {
    Metrics *m = arg;
    http_io_serve(m->lfd, &m->stop, handle, m->flow);
    return NULL;
}

/* ---------- public API ---------- */

int metrics_start(Flow *f, int port) {
    if (!f || port <= 0 || g_m.running) return 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("metrics: socket"); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_port        = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("metrics: bind"); close(s); return -1;
    }
    if (listen(s, 16) < 0) {
        perror("metrics: listen"); close(s); return -1;
    }

    g_m.flow = f;
    g_m.lfd  = s;
    g_m.port = port;
    g_m.stop = 0;
    if (pthread_create(&g_m.thr, NULL, metrics_main, &g_m) != 0) {
        perror("metrics: pthread_create");
        close(s); g_m.lfd = -1;
        return -1;
    }
    g_m.running = 1;
    fprintf(stderr, "jerboa: metrics on http://127.0.0.1:%d/metrics\n", port);
    return 0;
}

void metrics_stop(void) {
    if (!g_m.running) return;
    g_m.stop = 1;
    /* Unblock any in-flight accept/recv so the thread sees stop promptly. */
    if (g_m.lfd >= 0) shutdown(g_m.lfd, SHUT_RDWR);
    pthread_join(g_m.thr, NULL);
    close(g_m.lfd);
    g_m.lfd = -1;
    g_m.running = 0;
}
