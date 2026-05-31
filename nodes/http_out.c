/* nodes/http_out.c -- HTTP/1.0 client sink (also re-emits response body).
 *
 * Config:
 *   <name> http_out <url> [method] [content-type]
 *
 *   url           http://host[:port]/path   (no TLS, no redirects)
 *   method        default POST
 *   content-type  default application/octet-stream
 *
 * Per input packet:
 *   - delegate the full request/response round-trip to http_client_do
 *     (DNS, connect, send, drain, parse status + body),
 *   - log non-2xx, malformed, or truncated responses,
 *   - if the node has any outputs wired, emit the response body
 *     (capped at MAX_RESP) as a new packet.
 *
 * Plain HTTP only -- if you need TLS, put a reverse proxy (nginx,
 * haproxy) in front of the target. */

#define _POSIX_C_SOURCE 200809L

#include "nodes.h"
#include "../http_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_TIMEOUT_S   5
#define MAX_RESP       (1u << 20)           /* 1 MiB body cap */
#define RAW_CAP        (MAX_RESP + 8192)    /* body cap + header slack */

typedef struct {
    HttpUrl url;
    char    method[16];
    char    ctype[128];
} Ctx;

/* ---------- process ---------- */

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    Ctx *c = self->ctx;

    HttpResponse r;
    if (http_client_do(&c->url, c->method, c->ctype,
                       in->data, in->len,
                       RAW_CAP, IO_TIMEOUT_S, self->name, &r) < 0)
        return NULL;

    if (r.status < 0)
        fprintf(stderr, "jerboa: http_out/%s: malformed response from %s:%s\n",
                self->name, c->url.host, c->url.port);
    else if (r.status < 200 || r.status >= 300)
        fprintf(stderr, "jerboa: http_out/%s: %s %s -> %d\n",
                self->name, c->method, c->url.path, r.status);
    if (r.truncated)
        fprintf(stderr, "jerboa: http_out/%s: response truncated at %u bytes\n",
                self->name, MAX_RESP);

    Packet *out = NULL;
    if (self->n_out && r.body && r.body_len) {
        size_t n = r.body_len > MAX_RESP ? MAX_RESP : r.body_len;
        out = packet_new(r.body, n);
        if (!out)
            fprintf(stderr, "jerboa: http_out/%s: oom emitting response\n", self->name);
    }
    http_response_free(&r);
    return out;
}

/* ---------- init / free ---------- */

static void ctx_free(void *vp) { free(vp); }

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: http_out needs <url> [method] [content-type]\n");
        return -1;
    }

    /* Tokenize into up to 3 fields in a scratch copy. */
    char scratch[1024];
    if (strlen(args) >= sizeof(scratch)) {
        fprintf(stderr, "jerboa: http_out: args too long\n");
        return -1;
    }
    memcpy(scratch, args, strlen(args) + 1);

    char *save = NULL;
    char *url    = strtok_r(scratch, " \t", &save);
    char *method = strtok_r(NULL,    " \t", &save);
    char *ctype  = strtok_r(NULL,    "",    &save);
    if (!url) {
        fprintf(stderr, "jerboa: http_out: missing url\n");
        return -1;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    if (http_url_parse(url, &c->url, "http_out") < 0) { free(c); return -1; }

    if (method) {
        size_t L = strlen(method);
        if (L == 0 || L >= sizeof(c->method)) {
            fprintf(stderr, "jerboa: http_out: bad method\n");
            free(c); return -1;
        }
        for (size_t i = 0; i < L; i++) {
            unsigned char ch = (unsigned char)method[i];
            if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32);
            if (ch < 'A' || ch > 'Z') {
                fprintf(stderr, "jerboa: http_out: bad method\n");
                free(c); return -1;
            }
            c->method[i] = (char)ch;
        }
        c->method[L] = '\0';
    } else {
        memcpy(c->method, "POST", 5);
    }

    if (ctype) {
        while (*ctype == ' ' || *ctype == '\t') ctype++;
        size_t L = strlen(ctype);
        while (L && (ctype[L-1] == ' ' || ctype[L-1] == '\t')) L--;
        if (L == 0 || L >= sizeof(c->ctype)) {
            fprintf(stderr, "jerboa: http_out: bad content-type\n");
            free(c); return -1;
        }
        memcpy(c->ctype, ctype, L); c->ctype[L] = '\0';
    } else {
        memcpy(c->ctype, "application/octet-stream",
               sizeof("application/octet-stream"));
    }

    n->process  = process;
    n->ctx      = c;
    n->ctx_free = ctx_free;
    return 0;
}

const NodeType ndt_http_out = { "http_out", init };
