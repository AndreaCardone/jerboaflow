/* nodes/batch.c -- accumulate N payloads (joined with '\n') then emit one.
 * args: "<N>" (default 10).
 * NOTE: a partial batch in flight at shutdown is dropped. */
#include "nodes.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t target;
    size_t have;
    char  *buf;
    size_t len, cap;
} Ctx;

static int reserve(Ctx *c, size_t add) {
    if (c->len + add <= c->cap) return 0;
    size_t cap = c->cap ? c->cap * 2 : 256;
    while (cap < c->len + add) cap *= 2;
    char *p = realloc(c->buf, cap);
    if (!p) return -1;
    c->buf = p;
    c->cap = cap;
    return 0;
}

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    size_t add = in->len + (c->have ? 1 : 0);  /* leading '\n' between entries */
    if (reserve(c, add) < 0) { packet_release(in); return NULL; }
    if (c->have) c->buf[c->len++] = '\n';
    memcpy(c->buf + c->len, in->data, in->len);
    c->len += in->len;
    c->have++;
    packet_release(in);
    if (c->have < c->target) return NULL;
    Packet *out = packet_new(c->buf, c->len);
    c->have = 0;
    c->len  = 0;
    return out;
}

static void ctx_free(void *p) {
    Ctx *c = p;
    free(c->buf);
    free(c);
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->target = 10;
    if (args && *args) {
        long v = strtol(args, NULL, 10);
        if (v > 0) c->target = (size_t)v;
    }
    n->ctx = c;
    n->ctx_free = ctx_free;
    n->process = process;
    return 0;
}

const NodeType ndt_batch = { "batch", init };
