/* nodes/random.c -- replace each input with a random integer in [min, max].
 * args: "<min> <max>" */
#include "nodes.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    long         lo;
    long         hi;
    unsigned int seed;
} Ctx;

static long next_long(Ctx *c) {
    unsigned long span = (unsigned long)(c->hi - c->lo) + 1UL;
    return c->lo + (long)(rand_r(&c->seed) % span);
}

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    packet_release(in);

    long v = next_long(c);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld", v);
    if (n < 0) return NULL;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    return packet_new(buf, (size_t)n);
}

static int init(Node *n, const char *args) {
    long lo = 0;
    long hi = 100;
    if (args && *args) {
        if (sscanf(args, "%ld %ld", &lo, &hi) != 2) {
            fprintf(stderr, "jerboa: random needs: <min> <max>\n");
            return -1;
        }
    }

    if (lo > hi) {
        long t = lo;
        lo = hi;
        hi = t;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->lo = lo;
    c->hi = hi;
    c->seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)c;

    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_random = { "random", init };