/* nodes/throttle.c -- forward at most one packet every <interval_ms> ms, drop rest.
 * args: "<interval_ms>" (default 1000) */
#define _POSIX_C_SOURCE 200809L
#include "nodes.h"
#include <stdlib.h>
#include <time.h>

typedef struct { long interval_ms; long last_ms; } Ctx;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    long t = now_ms();
    if (t - c->last_ms >= c->interval_ms) {
        c->last_ms = t;
        return in;
    }
    packet_release(in);
    return NULL;
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->interval_ms = 1000;
    if (args && *args) {
        long v = strtol(args, NULL, 10);
        if (v > 0) c->interval_ms = v;
    }
    c->last_ms = 0;
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_throttle = { "throttle", init };
