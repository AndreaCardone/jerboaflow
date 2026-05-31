/* nodes/epoll_in.c -- line-oriented source watched via epoll(7).
 *
 * Config:
 *   <name> epoll_in <path>
 *
 * Opens <path> non-blocking, watches it with epoll, and emits one
 * packet per '\n'-terminated line. CRLF tolerant (trailing '\r'
 * stripped). Lines longer than MAX_LINE are flushed as one packet
 * with a stderr warning -- bytes are never silently dropped.
 *
 * Supported paths: anything epoll accepts -- FIFOs, char devices,
 * sockets, pipes. Regular files are NOT supported: epoll_ctl returns
 * EPERM for them on Linux (they're always "readable"). Use inotify
 * or a tail-style poller for log files.
 *
 * FIFOs are opened O_RDWR so we always hold a writer ourselves; this
 * means external writer churn (open/write/close cycles) does not
 * terminate the listener. Char devices stay O_RDONLY.
 *
 * Listener thread blocks in epoll_wait with a 200 ms tick so on_stop
 * is observed promptly. Shutdown idiom matches http_in: on_stop sets
 * stop=1 and closes the inbox; process() returns NULL when the inbox
 * is closed-and-empty. */

#define _POSIX_C_SOURCE 200809L

#include "nodes.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

#define EPI_INBOX_CAP    1024            /* power of two */
#define EPI_MAX_LINE     4096
#define EPI_READ_CHUNK   4096
#define EPI_TICK_MS      200

typedef struct {
    char           path[256];
    int            fd;
    int            ep;

    /* Reassembly: bytes seen since the last '\n'. Listener-only, no lock. */
    unsigned char  acc[EPI_MAX_LINE];
    size_t         acc_len;

    pthread_t      thr;
    int            has_thread;
    atomic_int     stop;
    PQueue        *inbox;
    Node          *self;
} Ctx;

/* ---------- framer ---------- */

/* Push acc[..acc_len] (with optional trailing \r stripped) as a packet.
 * Returns 0 on success, -1 if the inbox was closed (shutdown). */
static int flush_line(Ctx *c) {
    size_t n = c->acc_len;
    if (n && c->acc[n - 1] == '\r') n--;
    Packet *p = packet_new(c->acc, n);
    c->acc_len = 0;
    if (!p) return 0;                        /* OOM: drop, keep listening */
    if (pqueue_push(c->inbox, p) != 0) {
        packet_release(p);
        return -1;
    }
    return 0;
}

/* Feed `len` bytes into the framer. Returns 0 normally, -1 on shutdown. */
static int feed(Ctx *c, const unsigned char *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        const unsigned char *nl = memchr(buf + i, '\n', len - i);
        size_t chunk = nl ? (size_t)(nl - (buf + i)) : (len - i);

        /* Copy as much as fits; if acc overflows without a newline,
         * flush what we have as one (oversized) line and warn. */
        size_t room = EPI_MAX_LINE - c->acc_len;
        size_t take = chunk < room ? chunk : room;
        memcpy(c->acc + c->acc_len, buf + i, take);
        c->acc_len += take;
        i += take;

        if (take < chunk) {
            /* acc is full and we still haven't hit '\n'. */
            fprintf(stderr,
                "jerboa: epoll_in/%s: line exceeds %u bytes, flushing partial\n",
                c->self->name, EPI_MAX_LINE);
            if (flush_line(c) < 0) return -1;
            continue;                        /* loop will copy the rest */
        }
        if (nl) {
            i++;                             /* consume the '\n' */
            if (flush_line(c) < 0) return -1;
        }
    }
    return 0;
}

/* ---------- listener thread ---------- */

static void *listener_main(void *arg) {
    Ctx *c = arg;
    while (!c->stop) {
        struct epoll_event ev;
        int r = epoll_wait(c->ep, &ev, 1, EPI_TICK_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("epoll_in: epoll_wait");
            break;
        }
        if (r == 0) continue;
        if (ev.events & (EPOLLERR | EPOLLHUP)) {
            /* Try one final drain in case data is still readable. */
        }

        /* Drain to EAGAIN. UART/FIFO buffers are tiny -- empty the
         * kernel buffer before sleeping again. */
        for (;;) {
            unsigned char buf[EPI_READ_CHUNK];
            ssize_t n = read(c->fd, buf, sizeof(buf));
            if (n > 0) {
                if (feed(c, buf, (size_t)n) < 0) goto out;
                continue;
            }
            if (n == 0) {                    /* writer closed / EOF */
                fprintf(stderr,
                    "jerboa: epoll_in/%s: EOF on %s, listener exiting\n",
                    c->self->name, c->path);
                goto out;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fprintf(stderr,
                "jerboa: epoll_in/%s: read %s: %s\n",
                c->self->name, c->path, strerror(errno));
            goto out;
        }
    }
out:
    return NULL;
}

/* ---------- node hooks ---------- */

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx; (void)in;
    Ctx *c = self->ctx;
    return pqueue_pop(c->inbox);
}

static void on_stop(Node *self) {
    Ctx *c = self->ctx;
    c->stop = 1;
    if (c->inbox) pqueue_close(c->inbox);
}

static void ctx_free(void *vp) {
    Ctx *c = vp;
    if (c->has_thread) {
        c->stop = 1;
        if (c->inbox) pqueue_close(c->inbox);
        pthread_join(c->thr, NULL);
    }
    if (c->ep >= 0) close(c->ep);
    if (c->fd >= 0) close(c->fd);
    if (c->inbox) {
        Packet *p;
        while (pqueue_trypop(c->inbox, &p) == 0) packet_release(p);
        pqueue_free(c->inbox);
    }
    free(c);
}

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: epoll_in needs <path>\n");
        return -1;
    }
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->fd = c->ep = -1;
    c->self = n;

    if (sscanf(args, "%255s", c->path) != 1) {
        fprintf(stderr, "jerboa: epoll_in: bad path\n");
        free(c); return -1;
    }

    /* Peek at the path's type to choose open mode. For FIFOs we want
     * O_RDWR so external writer churn doesn't EOF us. For everything
     * else, O_RDONLY. We can't fstat the fd-before-open, so stat the
     * path; this is a TOCTOU sliver but the node is a long-lived
     * source -- if the path swaps under us we'd fail open anyway. */
    struct stat st;
    int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
    if (stat(c->path, &st) == 0 && S_ISFIFO(st.st_mode))
        flags = O_RDWR | O_NONBLOCK | O_CLOEXEC;

    c->fd = open(c->path, flags);
    if (c->fd < 0) {
        fprintf(stderr, "jerboa: epoll_in: open %s: %s\n",
                c->path, strerror(errno));
        free(c); return -1;
    }

    c->ep = epoll_create1(EPOLL_CLOEXEC);
    if (c->ep < 0) {
        perror("epoll_in: epoll_create1");
        close(c->fd); free(c); return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = c->fd };
    if (epoll_ctl(c->ep, EPOLL_CTL_ADD, c->fd, &ev) < 0) {
        fprintf(stderr,
            "jerboa: epoll_in: epoll_ctl %s: %s (regular files not supported)\n",
            c->path, strerror(errno));
        close(c->ep); close(c->fd); free(c); return -1;
    }

    c->inbox = pqueue_new(EPI_INBOX_CAP);
    if (!c->inbox) {
        close(c->ep); close(c->fd); free(c); return -1;
    }

    if (pthread_create(&c->thr, NULL, listener_main, c) != 0) {
        pqueue_free(c->inbox);
        close(c->ep); close(c->fd); free(c); return -1;
    }
    c->has_thread = 1;

    n->ctx             = c;
    n->ctx_free        = ctx_free;
    n->process         = process;
    n->on_stop         = on_stop;
    n->src_interval_ms = 0;
    return 0;
}

const NodeType ndt_epoll_in = { "epoll_in", init };
