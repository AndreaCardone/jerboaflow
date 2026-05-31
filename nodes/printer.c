/* nodes/printer.c -- write payload to stdout, prefixed with node + port.
 * args: "<prefix>" (default "out") */
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { char prefix[32]; } Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    Ctx *c = self->ctx;
    if (!in) return NULL;
    /* Hold the FILE lock across all writes so concurrent printers on
     * different worker threads cannot interleave inside a single line. */
    flockfile(stdout);
    fprintf(stdout, "[%s/%s in=%zu] ", self->name, c ? c->prefix : "out", idx);
    fwrite(in->data, 1, in->len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
    packet_release(in);
    return NULL;
}

static int init(Node *n, const char *args) {
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    const char *src = (args && *args) ? args : "out";
    size_t i = 0;
    for (; src[i] && i < sizeof(c->prefix) - 1; i++) c->prefix[i] = src[i];
    c->prefix[i] = '\0';
    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_printer = { "printer", init };
