/* nodes/count.c -- increment a counter on every input, emit decimal as text. */
#include "nodes.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint64_t n; } Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    Ctx *c = self->ctx;
    if (!in) return NULL;
    packet_release(in);
    c->n++;
    char buf[32];
    int k = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)c->n);
    if (k < 0) return NULL;
    return packet_new(buf, (size_t)k);
}

static int init(Node *n, const char *args) {
    (void)args;
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_count = { "count", init };
