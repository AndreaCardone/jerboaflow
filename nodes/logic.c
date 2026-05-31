/* nodes/logic.c -- compare payload, then forward, drop, or emit text.
 * args: "<op> <compare> <true_action> <false_action>"
 * 
 * Actions:
 *   =   forward original packet
 *   !   drop packet
 *   txt emit literal token `txt`
 *
 * Operators:
 *   == != < <= > >= contains
 */
#include "nodes.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_CONTAINS,
} LogicOp;

typedef enum {
    ACT_FORWARD,
    ACT_DROP,
    ACT_EMIT,
} ActionKind;

typedef struct {
    ActionKind kind;
    char       text[128];
    size_t     len;
} Action;

typedef struct {
    LogicOp op;
    char    rhs[128];
    size_t  rhs_len;
    Action  on_true;
    Action  on_false;
} Ctx;

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static int parse_token(const char **ps, char *dst, size_t cap) {
    const char *s = skip_ws(*ps);
    size_t n = 0;
    int quoted = 0;

    if (!*s) return -1;
    if (*s == '"') {
        quoted = 1;
        s++;
    }

    while (*s) {
        if (quoted) {
            if (*s == '"') {
                s++;
                break;
            }
            if (*s == '\\' && s[1]) s++;
        } else if (isspace((unsigned char)*s)) {
            break;
        }

        if (n + 1 >= cap) return -1;
        dst[n++] = *s++;
    }

    if (quoted && s[-1] != '"') return -1;
    dst[n] = '\0';
    *ps = skip_ws(s);
    return 0;
}

static int bytes_cmp(const void *ap, size_t alen, const void *bp, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int rc = memcmp(ap, bp, n);
    if (rc != 0) return rc;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static int bytes_contains(const void *hay, size_t hlen, const void *needle, size_t nlen) {
    const unsigned char *h = hay;
    if (nlen == 0) return 1;
    if (hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(h + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static int parse_op(const char *s, LogicOp *op) {
    if (strcmp(s, "==") == 0) *op = OP_EQ;
    else if (strcmp(s, "!=") == 0) *op = OP_NE;
    else if (strcmp(s, "<") == 0) *op = OP_LT;
    else if (strcmp(s, "<=") == 0) *op = OP_LE;
    else if (strcmp(s, ">") == 0) *op = OP_GT;
    else if (strcmp(s, ">=") == 0) *op = OP_GE;
    else if (strcmp(s, "contains") == 0) *op = OP_CONTAINS;
    else return -1;
    return 0;
}

static void parse_action(Action *a, const char *s) {
    if (strcmp(s, "=") == 0) {
        a->kind = ACT_FORWARD;
        return;
    }
    if (strcmp(s, "!") == 0) {
        a->kind = ACT_DROP;
        return;
    }
    a->kind = ACT_EMIT;
    size_t i = 0;
    for (; s[i] && i < sizeof(a->text) - 1; i++) a->text[i] = s[i];
    a->text[i] = '\0';
    a->len = i;
}

static int eval(const Ctx *c, const Packet *in) {
    int rc = bytes_cmp(in->data, in->len, c->rhs, c->rhs_len);
    switch (c->op) {
    case OP_EQ:       return rc == 0;
    case OP_NE:       return rc != 0;
    case OP_LT:       return rc < 0;
    case OP_LE:       return rc <= 0;
    case OP_GT:       return rc > 0;
    case OP_GE:       return rc >= 0;
    case OP_CONTAINS: return bytes_contains(in->data, in->len, c->rhs, c->rhs_len);
    }
    return 0;
}

static Packet *apply_action(const Action *a, Packet *in) {
    if (a->kind == ACT_FORWARD) return in;
    packet_release(in);
    if (a->kind == ACT_DROP) return NULL;
    return packet_new(a->text, a->len);
}

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    return apply_action(eval(c, in) ? &c->on_true : &c->on_false, in);
}

static int init(Node *n, const char *args) {
    char op_s[16] = {0};
    char rhs[128] = {0};
    char on_true[128] = {0};
    char on_false[128] = {0};
    const char *p = args;
    if (!args ||
        parse_token(&p, op_s, sizeof(op_s)) != 0 ||
        parse_token(&p, rhs, sizeof(rhs)) != 0 ||
        parse_token(&p, on_true, sizeof(on_true)) != 0 ||
        parse_token(&p, on_false, sizeof(on_false)) != 0 ||
        *p != '\0') {
        fprintf(stderr, "jerboa: logic needs: <op> <compare> <true_action> <false_action>\n");
        return -1;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    if (parse_op(op_s, &c->op) != 0) {
        fprintf(stderr, "jerboa: logic unknown op '%s'\n", op_s);
        free(c);
        return -1;
    }

    size_t i = 0;
    for (; rhs[i] && i < sizeof(c->rhs) - 1; i++) c->rhs[i] = rhs[i];
    c->rhs[i] = '\0';
    c->rhs_len = i;
    parse_action(&c->on_true, on_true);
    parse_action(&c->on_false, on_false);

    n->ctx = c;
    n->ctx_free = free;
    n->process = process;
    return 0;
}

const NodeType ndt_logic = { "logic", init };