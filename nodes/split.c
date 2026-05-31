/* nodes/split.c -- split incoming payload on a single-byte delimiter,
 * emit one packet per non-empty piece (uses node_emit).
 * args: "<delim>"  (default '\n'; first char of args is taken) */
#include "nodes.h"
#include <stdlib.h>

typedef struct { char delim; } Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    const char *p = in->data;
    size_t len = in->len;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || p[i] == c->delim) {
            if (i > start) {
                Packet *out = packet_new(p + start, i - start);
                if (out) node_emit(self, out);
            }
            start = i + 1;
        }
    }
    packet_release(in);
    return NULL;   /* all emission already done */
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->delim = (args && args[0]) ? args[0] : '\n';
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_split = { "split", init };
