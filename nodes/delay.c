/* nodes/delay.c -- forward each packet after sleeping <interval_ms> ms.
 * args: "<interval_ms>" (default 1000)
 *
 * Blocking: occupies the calling worker for the duration. Order is
 * preserved on a single input; across inputs it depends on arrival
 * order. Unlike `throttle`, no packets are dropped.
 */
#define _POSIX_C_SOURCE 200809L
#include "nodes.h"
#include <stdlib.h>
#include <time.h>

typedef struct { long interval_ms; } Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    struct timespec ts = {
        .tv_sec  = c->interval_ms / 1000,
        .tv_nsec = (c->interval_ms % 1000) * 1000000L,
    };
    while (nanosleep(&ts, &ts) == -1) { /* resume on EINTR */ }
    return in;
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->interval_ms = 1000;
    if (args && *args) {
        long v = strtol(args, NULL, 10);
        if (v > 0) c->interval_ms = v;
    }
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_delay = { "delay", init };
