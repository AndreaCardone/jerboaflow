/* nodes/generator.c -- periodic text-packet source.
 * args: "<interval_ms> <count> <text>"   (count==0 means infinite) */
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    char     text[64];
    unsigned remaining;
    uint64_t seq;
} Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx; (void)in;
    Ctx *c = self->ctx;
    if (c->remaining != 0 && c->seq >= c->remaining) return NULL;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s-%llu",
                     c->text, (unsigned long long)c->seq);
    if (n < 0) return NULL;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    c->seq++;
    return packet_new(buf, (size_t)n);
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    unsigned interval = 1000, count = 0;
    char text[64] = "tick";
    if (args && *args) sscanf(args, "%u %u %63s", &interval, &count, text);
    size_t i = 0;
    for (; text[i] && i < sizeof(c->text) - 1; i++) c->text[i] = text[i];
    c->text[i] = '\0';
    c->remaining = count;
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    n->src_interval_ms = interval;
    return 0;
}

const NodeType ndt_generator = { "generator", init };
