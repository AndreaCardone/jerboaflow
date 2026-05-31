/* nodes/filter_re.c -- forward only packets whose payload matches a POSIX
 * extended regular expression.
 *
 * args: "<regex>"
 *
 * The packet bytes are matched as a NUL-terminated string against the
 * pattern compiled with REG_EXTENDED | REG_NOSUB. To stay safe against
 * embedded NULs in binary payloads (regexec would stop early), the
 * payload is copied into a small stack buffer when it fits and into a
 * heap buffer otherwise; any NUL byte inside is rewritten to '\x01'
 * before matching -- regex authors don't write patterns that depend on
 * NUL, and the substitution preserves length and surrounding context.
 */

#include "nodes.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_BUF  4096

typedef struct {
    regex_t re;
    int     compiled;
} Ctx;

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;

    char  stack[STACK_BUF];
    char *buf = stack;
    if (in->len + 1 > sizeof(stack)) {
        buf = malloc(in->len + 1);
        if (!buf) { packet_release(in); return NULL; }
    }
    memcpy(buf, in->data, in->len);
    buf[in->len] = '\0';
    for (size_t i = 0; i < in->len; i++) if (buf[i] == '\0') buf[i] = '\x01';

    int rc = regexec(&c->re, buf, 0, NULL, 0);
    if (buf != stack) free(buf);

    if (rc == 0) return in;
    packet_release(in);
    return NULL;
}

static void ctx_free(void *p) {
    Ctx *c = p;
    if (c->compiled) regfree(&c->re);
    free(c);
}

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: filter_re: missing regex\n");
        return -1;
    }
    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    int rc = regcomp(&c->re, args, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char err[128];
        regerror(rc, &c->re, err, sizeof(err));
        fprintf(stderr, "jerboa: filter_re: bad regex '%s': %s\n", args, err);
        free(c);
        return -1;
    }
    c->compiled = 1;
    n->ctx = c;
    n->ctx_free = ctx_free;
    n->process = process;
    return 0;
}

const NodeType ndt_filter_re = { "filter_re", init };
