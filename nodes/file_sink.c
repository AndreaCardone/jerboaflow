/* nodes/file_sink.c -- append payload + '\n' to a file. args: "<path>" */
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { FILE *fp; } Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    fwrite(in->data, 1, in->len, c->fp);
    fputc('\n', c->fp);
    fflush(c->fp);
    packet_release(in);
    return NULL;
}

static void ctx_free(void *p) {
    Ctx *c = p;
    if (c->fp) fclose(c->fp);
    free(c);
}

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: fwriter needs a path\n");
        return -1;
    }
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->fp = fopen(args, "w");
    if (!c->fp) { perror("fwriter: fopen"); free(c); return -1; }
    n->ctx = c;
    n->ctx_free = ctx_free;
    n->process = process;
    return 0;
}

const NodeType ndt_fwriter = { "fwriter", init };
