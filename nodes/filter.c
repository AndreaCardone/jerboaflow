/* nodes/filter.c -- forward only packets whose payload contains <needle>.
 * args: "<needle>" (substring; max 63 bytes; NUL-terminated for memmem) */
#define _GNU_SOURCE
#include "nodes.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char needle[64]; size_t nlen; } Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    if (c->nlen == 0 || (in->len >= c->nlen &&
                         memmem(in->data, in->len, c->needle, c->nlen) != NULL))
        return in;
    packet_release(in);
    return NULL;
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    if (args && *args) {
        size_t i = 0;
        for (; args[i] && i < sizeof(c->needle) - 1; i++) c->needle[i] = args[i];
        c->needle[i] = '\0';
        c->nlen = i;
    }
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_filter = { "filter", init };
