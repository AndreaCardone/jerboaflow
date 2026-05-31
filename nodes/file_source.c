/* nodes/file_source.c -- read a file line by line, one packet per line, then EOF.
 * Acts as a source (n_in == 0). args: "<path>" */
#define _POSIX_C_SOURCE 200809L
#include "nodes.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    FILE   *fp;
    char   *line;
    size_t  cap;
} Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx; (void)in;
    Ctx *c = self->ctx;
    ssize_t n = getline(&c->line, &c->cap, c->fp);
    if (n < 0) return NULL;            /* EOF -> source exits */
    /* drop trailing newline if present */
    while (n > 0 && (c->line[n-1] == '\n' || c->line[n-1] == '\r')) n--;
    return packet_new(c->line, (size_t)n);
}

static void ctx_free(void *p) {
    Ctx *c = p;
    if (c->fp) fclose(c->fp);
    free(c->line);
    free(c);
}

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: file source needs a path\n");
        return -1;
    }
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->fp = fopen(args, "r");
    if (!c->fp) { perror("file source: fopen"); free(c); return -1; }
    n->ctx = c;
    n->ctx_free = ctx_free;
    n->process = process;
    n->src_interval_ms = 0;        /* read as fast as the pipeline drains */
    return 0;
}

const NodeType ndt_file = { "file", init };
