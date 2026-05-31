/* jerboa.c -- runtime implementation. */
#define _POSIX_C_SOURCE 200809L

#include "jerboa.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 *  Packet
 * ============================================================ */

Packet *packet_new(const void *src, size_t len) {
    Packet *p = malloc(sizeof(*p) + len);
    if (!p) return NULL;
    p->data = (len > 0) ? (char *)(p + 1) : NULL;
    p->len = len;
    atomic_init(&p->refcount, 1);
    if (len > 0) {
        if (src) memcpy(p->data, src, len);
        else     memset(p->data, 0, len);
    }
    return p;
}

void packet_retain(Packet *p) {
    if (!p) return;
    atomic_fetch_add_explicit(&p->refcount, 1, memory_order_relaxed);
}

void packet_release(Packet *p) {
    if (!p) return;
    if (atomic_fetch_sub_explicit(&p->refcount, 1, memory_order_acq_rel) == 1)
        free(p);
}

/* ============================================================
 *  Bounded MPMC packet queue (PQueue)
 * ============================================================ */

static size_t pow2_ceil(size_t n) {
    size_t r = 1;
    while (r < n) r <<= 1;
    return r < 2 ? 2 : r;
}

PQueue *pqueue_new(size_t cap) {
    cap = pow2_ceil(cap);
    PQueue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->buf = calloc(cap, sizeof(Packet *));
    if (!q->buf) { free(q); return NULL; }
    q->cap = cap;
    q->mask = cap - 1;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

void pqueue_free(PQueue *q) {
    if (!q) return;
    for (size_t i = 0; i < q->count; i++)
        packet_release(q->buf[(q->head + i) & q->mask]);
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
    free(q);
}

int pqueue_push(PQueue *q, Packet *p) {
    if (!p) return -1;  /* refuse NULL: would alias closed-queue sentinel in pop */
    pthread_mutex_lock(&q->mtx);
    while (q->count == q->cap && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mtx);
    if (q->closed) {
        q->m_dropped++;
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    q->buf[q->tail] = p;
    q->tail = (q->tail + 1) & q->mask;
    q->count++;
    q->m_pushed++;
    if (q->count > q->m_highwater) q->m_highwater = q->count;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

Packet *pqueue_pop(PQueue *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mtx);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return NULL;
    }
    Packet *p = q->buf[q->head];
    q->head = (q->head + 1) & q->mask;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return p;
}

int pqueue_trypop(PQueue *q, Packet **out) {
    pthread_mutex_lock(&q->mtx);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) & q->mask;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

size_t pqueue_len(PQueue *q) {
    pthread_mutex_lock(&q->mtx);
    size_t n = q->count;
    pthread_mutex_unlock(&q->mtx);
    return n;
}

int pqueue_done(PQueue *q) {
    pthread_mutex_lock(&q->mtx);
    int d = q->closed && q->count == 0;
    pthread_mutex_unlock(&q->mtx);
    return d;
}

void pqueue_close(PQueue *q) {
    pthread_mutex_lock(&q->mtx);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

/* ============================================================
 *  Node ready-queue (NQueue)
 * ============================================================ */

NQueue *nqueue_new(size_t cap) {
    cap = pow2_ceil(cap);
    NQueue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->buf = calloc(cap, sizeof(Node *));
    if (!q->buf) { free(q); return NULL; }
    q->cap = cap;
    q->mask = cap - 1;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

void nqueue_free(NQueue *q) {
    if (!q) return;
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
    free(q);
}

int nqueue_push(NQueue *q, Node *n) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == q->cap && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mtx);
    if (q->closed) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    q->buf[q->tail] = n;
    q->tail = (q->tail + 1) & q->mask;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

Node *nqueue_pop(NQueue *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mtx);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return NULL;
    }
    Node *n = q->buf[q->head];
    q->head = (q->head + 1) & q->mask;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return n;
}

void nqueue_close(NQueue *q) {
    pthread_mutex_lock(&q->mtx);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

/* ============================================================
 *  Scheduling
 * ============================================================ */

void flow_schedule(Flow *f, Node *n) {
    if (!f || !n || n->n_in == 0) return;
    pthread_mutex_lock(&n->state_mtx);
    int enqueue = 0;
    if (n->terminated) {
        /* nothing */
    } else if (n->running) {
        /* Worker is mid-process and may have already past its post-process
         * input check. Tell it to re-check before clearing running. */
        n->needs_rescan = 1;
    } else if (!n->scheduled) {
        n->scheduled = 1;
        enqueue = 1;
    }
    pthread_mutex_unlock(&n->state_mtx);
    if (enqueue) nqueue_push(f->ready, n);
}

static void alive_dec(Flow *f) {
    pthread_mutex_lock(&f->alive_mtx);
    int v = --f->alive;
    pthread_mutex_unlock(&f->alive_mtx);
    if (v == 0) nqueue_close(f->ready);
}

void node_emit(Node *n, Packet *out) {
    if (!out) return;
    atomic_fetch_add_explicit(&n->m_pkts_out, 1, memory_order_relaxed);
    if (n->n_out == 0) { packet_release(out); return; }
    /* one ref already held; add n_out-1 more so each push owns one */
    for (size_t i = 1; i < n->n_out; i++) packet_retain(out);
    for (size_t i = 0; i < n->n_out; i++) {
        if (pqueue_push(n->out[i], out) < 0)
            packet_release(out);
        else
            flow_schedule(n->flow, n->out[i]->consumer);
    }
}

static int all_inputs_done(Node *n) {
    for (size_t i = 0; i < n->n_in; i++)
        if (!pqueue_done(n->in[i])) return 0;
    return 1;
}

static int any_input_ready(Node *n) {
    for (size_t i = 0; i < n->n_in; i++)
        if (pqueue_len(n->in[i]) > 0) return 1;
    return 0;
}

/* ============================================================
 *  Worker thread
 * ============================================================ */

static void *worker_main(void *arg) {
    Flow *f = arg;
    for (;;) {
        Node *n = nqueue_pop(f->ready);
        if (!n) return NULL;

        pthread_mutex_lock(&n->state_mtx);
        n->scheduled = 0;
        n->running   = 1;
        pthread_mutex_unlock(&n->state_mtx);

        /* round-robin scan for a non-empty input */
        Packet *pkt = NULL;
        size_t  idx = 0;
        for (size_t i = 0; i < n->n_in; i++) {
            size_t k = (n->rr_cursor + i) % n->n_in;
            if (pqueue_trypop(n->in[k], &pkt) == 0) {
                idx = k;
                n->rr_cursor = (k + 1) % n->n_in;
                break;
            }
        }

        Packet *out = NULL;
        if (pkt) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            out = n->process(n, idx, pkt);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL
                        + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
            atomic_fetch_add_explicit(&n->m_pkts_in,       1,  memory_order_relaxed);
            atomic_fetch_add_explicit(&n->m_process_calls, 1,  memory_order_relaxed);
            atomic_fetch_add_explicit(&n->m_process_ns,    ns, memory_order_relaxed);
        }
        node_emit(n, out);

        int done = all_inputs_done(n);
        int has_more = any_input_ready(n);

        pthread_mutex_lock(&n->state_mtx);
        n->running = 0;
        int reschedule = 0;
        if (done) {
            n->terminated = 1;
        } else if (n->needs_rescan || has_more) {
            /* needs_rescan covers the race where flow_schedule fired after
             * our has_more check but before we took state_mtx. */
            n->needs_rescan = 0;
            if (!n->scheduled) { n->scheduled = 1; reschedule = 1; }
        }
        pthread_mutex_unlock(&n->state_mtx);

        if (done) {
            for (size_t i = 0; i < n->n_out; i++) {
                pqueue_close(n->out[i]);
                flow_schedule(f, n->out[i]->consumer);
            }
            alive_dec(f);
        } else if (reschedule) {
            nqueue_push(f->ready, n);
        }
    }
}

/* ============================================================
 *  Source thread
 * ============================================================ */

static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

static void *source_main(void *arg) {
    Node *n = arg;
    Flow *f = n->flow;
    while (!f->stop) {
        Packet *pkt = n->process(n, 0, NULL);
        if (!pkt) break;                 /* source exhausted */
        node_emit(n, pkt);
        if (n->src_interval_ms) sleep_ms(n->src_interval_ms);
    }
    for (size_t i = 0; i < n->n_out; i++) {
        pqueue_close(n->out[i]);
        flow_schedule(f, n->out[i]->consumer);
    }
    return NULL;
}

/* ============================================================
 *  Node type registry
 * ============================================================ */

#define MAX_NODE_TYPES 64
static const NodeType *g_types[MAX_NODE_TYPES];
static size_t          g_n_types = 0;
static pthread_mutex_t g_types_mtx = PTHREAD_MUTEX_INITIALIZER;

int node_register(const NodeType *t) {
    if (!t || !t->name || !t->init) return -1;
    pthread_mutex_lock(&g_types_mtx);
    int rc = 0;
    for (size_t i = 0; i < g_n_types; i++) {
        if (strcmp(g_types[i]->name, t->name) == 0) { goto out; }
    }
    if (g_n_types >= MAX_NODE_TYPES) { rc = -1; goto out; }
    g_types[g_n_types++] = t;
out:
    pthread_mutex_unlock(&g_types_mtx);
    return rc;
}

const NodeType *node_lookup(const char *name) {
    pthread_mutex_lock(&g_types_mtx);
    const NodeType *r = NULL;
    for (size_t i = 0; i < g_n_types; i++) {
        if (strcmp(g_types[i]->name, name) == 0) { r = g_types[i]; break; }
    }
    pthread_mutex_unlock(&g_types_mtx);
    return r;
}

/* ============================================================
 *  Configuration parser
 *
 *   Lines:
 *     <name> <type> [args...]                    (declaration)
 *     <src> -> <dst>[:<port>][, <dst>[:<port>]]  (edge)
 *     # comment
 * ============================================================ */

typedef struct EdgeSpec {
    char     src[32];
    char     dst[32];
    size_t   dst_port;
} EdgeSpec;

typedef struct NodeSpec {
    char     name[32];
    char     type[16];
    char     args[192];
} NodeSpec;

/* dynamic arrays */
typedef struct {
    NodeSpec *v; size_t n, cap;
} NodeVec;
typedef struct {
    EdgeSpec *v; size_t n, cap;
} EdgeVec;

static int nvec_push(NodeVec *a, NodeSpec s) {
    if (a->n == a->cap) {
        size_t c = a->cap ? a->cap * 2 : 8;
        NodeSpec *p = realloc(a->v, c * sizeof(*p));
        if (!p) return -1;
        a->v = p; a->cap = c;
    }
    a->v[a->n++] = s;
    return 0;
}

static int evec_push(EdgeVec *a, EdgeSpec s) {
    if (a->n == a->cap) {
        size_t c = a->cap ? a->cap * 2 : 8;
        EdgeSpec *p = realloc(a->v, c * sizeof(*p));
        if (!p) return -1;
        a->v = p; a->cap = c;
    }
    a->v[a->n++] = s;
    return 0;
}

/* Copy at most dstlen-1 bytes, NUL-terminate. Returns 1 if src was
 * truncated, 0 if it fit. */
static int str_copy(char *dst, size_t dstlen, const char *src) {
    if (dstlen == 0) return src && *src ? 1 : 0;
    size_t i = 0;
    for (; src[i] && i < dstlen - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
    return src[i] != '\0';
}

/* Trim leading/trailing whitespace in place; returns pointer into s. */
static char *str_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

/* Parse "name[:port]" -> name copied to out, port returned (default 0). */
static int parse_endpoint(const char *s, char *out, size_t outlen, size_t *port) {
    const char *colon = strchr(s, ':');
    *port = 0;
    if (!colon) {
        str_copy(out, outlen, s);
        return 0;
    }
    size_t nlen = (size_t)(colon - s);
    if (nlen >= outlen) return -1;
    memcpy(out, s, nlen);
    out[nlen] = '\0';
    char *end;
    long v = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || v < 0) return -1;
    *port = (size_t)v;
    return 0;
}

static int parse_line(char *line, NodeVec *nodes, EdgeVec *edges) {
    line = str_trim(line);
    if (!*line || *line == '#') return 0;

    char *arrow = strstr(line, "->");
    if (arrow) {
        *arrow = '\0';
        char *lhs = str_trim(line);
        char *rhs = str_trim(arrow + 2);
        EdgeSpec base;
        size_t dummy;
        if (parse_endpoint(lhs, base.src, sizeof(base.src), &dummy) < 0) return -1;
        /* rhs: comma-separated list of dst[:port] */
        char *save = NULL;
        for (char *tok = strtok_r(rhs, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            EdgeSpec e = base;
            char *t = str_trim(tok);
            if (parse_endpoint(t, e.dst, sizeof(e.dst), &e.dst_port) < 0) return -1;
            if (evec_push(edges, e) < 0) return -1;
        }
        return 0;
    }

    /* declaration: name type [args...] */
    NodeSpec s = {{0},{0},{0}};
    char *save = NULL;
    char *tok = strtok_r(line, " \t", &save);
    if (!tok) return -1;
    str_copy(s.name, sizeof(s.name), tok);
    tok = strtok_r(NULL, " \t", &save);
    if (!tok) return -1;
    str_copy(s.type, sizeof(s.type), tok);
    char *rest = strtok_r(NULL, "", &save);
    if (rest) str_copy(s.args, sizeof(s.args), str_trim(rest));
    return nvec_push(nodes, s);
}

/* ============================================================
 *  Wiring
 * ============================================================ */

static int find_node(Node *nodes, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(nodes[i].name, name) == 0) return (int)i;
    return -1;
}

static int set_node_type(Node *n, const NodeSpec *s) {
    const NodeType *t = node_lookup(s->type);
    if (!t) {
        fprintf(stderr, "jerboa: unknown node type '%s'\n", s->type);
        return -1;
    }
    return t->init(n, s->args);
}

#define EDGE_CAP 64  /* per-edge queue capacity */

/* ============================================================
 *  flow_load
 * ============================================================ */

/* Read an entire regular file into a malloc'd NUL-terminated buffer.
 * Refuses non-regular files (pipes, sockets) and verifies no short read,
 * so the parser cannot see a premature NUL spliced into the middle. */
static char *read_file_all(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size >= SIZE_MAX) {
        fclose(fp); errno = EFBIG; return NULL;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t off = 0;
    while (off < sz) {
        size_t want = sz - off;
        size_t got  = fread(buf + off, 1, want, fp);
        if (got == 0) {
            if (ferror(fp) && errno == EINTR) { clearerr(fp); continue; }
            fclose(fp); free(buf); return NULL;
        }
        off += got;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

/* Parse one config file into nv/ev. */
static int parse_file(const char *path, NodeVec *nv, EdgeVec *ev)
{
    char *text = read_file_all(path);
    if (!text) {
        fprintf(stderr, "jerboa: open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    int rc = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
                line = strtok_r(NULL, "\n", &save)) {
        char *t = str_trim(line);
        if (!*t || *t == '#') continue;
        if (parse_line(t, nv, ev) < 0) {
            fprintf(stderr, "jerboa: parse error in '%s'\n", path);
            rc = -1; break;
        }
    }

    free(text);
    return rc;
}

Flow *flow_load(const char *path, size_t n_workers) {
    NodeVec nv = {0}; EdgeVec ev = {0};
    Flow *f = NULL;
    size_t *fanin = NULL, *fanout = NULL, *out_cursor = NULL;

    if (parse_file(path, &nv, &ev) < 0) goto fail;

    f = calloc(1, sizeof(*f));
    if (!f) goto fail;
    pthread_mutex_init(&f->alive_mtx, NULL);

    if (nv.n) {
        f->nodes = calloc(nv.n, sizeof(Node));
        if (!f->nodes) goto fail;
    }
    f->n_nodes = nv.n;

    /* count fan-in / fan-out per node */
    fanin  = calloc(nv.n ? nv.n : 1, sizeof(size_t));
    fanout = calloc(nv.n ? nv.n : 1, sizeof(size_t));
    if (!fanin || !fanout) goto fail;

    for (size_t i = 0; i < ev.n; i++) {
        /* find by NodeSpec, since Node names not yet set */
        int s = -1, d = -1;
        for (size_t k = 0; k < nv.n; k++) {
            if (strcmp(nv.v[k].name, ev.v[i].src) == 0) s = (int)k;
            if (strcmp(nv.v[k].name, ev.v[i].dst) == 0) d = (int)k;
        }
        if (s < 0 || d < 0) {
            fprintf(stderr, "jerboa: edge references unknown node\n");
            goto fail;
        }
        fanout[s]++;
        if (ev.v[i].dst_port + 1 > fanin[d]) fanin[d] = ev.v[i].dst_port + 1;
    }

    /* initialize node structs and per-edge queues */
    for (size_t i = 0; i < nv.n; i++) {
        Node *n = &f->nodes[i];
        if (str_copy(n->name, sizeof(n->name), nv.v[i].name)) {
            fprintf(stderr, "jerboa: node name '%s' truncated to %zu chars\n",
                    nv.v[i].name, sizeof(n->name) - 1);
            goto fail;
        }
        if (str_copy(n->type, sizeof(n->type), nv.v[i].type)) {
            fprintf(stderr, "jerboa: node type '%s' truncated\n", nv.v[i].type);
            goto fail;
        }
        /* Reject duplicate names: find_node returns first hit so a dup
         * would silently shadow the second declaration. */
        for (size_t k = 0; k < i; k++) {
            if (strcmp(f->nodes[k].name, n->name) == 0) {
                fprintf(stderr, "jerboa: duplicate node name '%s'\n", n->name);
                goto fail;
            }
        }
        pthread_mutex_init(&n->state_mtx, NULL);
        n->flow = f;
        n->n_in  = fanin[i];
        n->n_out = fanout[i];
        if (n->n_in)  n->in  = calloc(n->n_in,  sizeof(PQueue *));
        if (n->n_out) n->out = calloc(n->n_out, sizeof(PQueue *));
        if ((n->n_in && !n->in) || (n->n_out && !n->out)) goto fail;
        if (set_node_type(n, &nv.v[i]) < 0) goto fail;
    }

    /* allocate queues: one per dst input port that has an edge. Each
     * (dst, dst_port) gets exactly one PQueue. */
    if (ev.n) {
        f->pqueues = calloc(ev.n, sizeof(PQueue *));
        if (!f->pqueues) goto fail;
    }

    /* per-node cursor to fill out[] in declaration order */
    out_cursor = calloc(nv.n ? nv.n : 1, sizeof(size_t));
    if (!out_cursor) goto fail;

    for (size_t i = 0; i < ev.n; i++) {
        int s = find_node(f->nodes, f->n_nodes, ev.v[i].src);
        int d = find_node(f->nodes, f->n_nodes, ev.v[i].dst);
        Node *dn = &f->nodes[d];
        Node *sn = &f->nodes[s];
        size_t port = ev.v[i].dst_port;

        PQueue *q = dn->in[port];
        if (!q) {
            q = pqueue_new(EDGE_CAP);
            if (!q) goto fail;
            q->consumer = dn;
            dn->in[port] = q;
            f->pqueues[f->n_pqueues++] = q;
        }
        sn->out[out_cursor[s]++] = q;
    }

    /* Sanity: every declared input port must have a queue. A NULL slot
     * means the user declared port N but never wired it; pop on a NULL
     * PQueue* would crash at first packet. */
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        for (size_t p = 0; p < n->n_in; p++) {
            if (!n->in[p]) {
                fprintf(stderr, "jerboa: node '%s' input port %zu has no incoming edge\n",
                        n->name, p);
                goto fail;
            }
        }
    }

    /* workers */
    if (n_workers == 0) {
        long np = sysconf(_SC_NPROCESSORS_ONLN);
        n_workers = (np > 0) ? (size_t)np : 2;
    }
    f->n_workers = n_workers;
    f->workers   = calloc(n_workers, sizeof(pthread_t));
    if (!f->workers) goto fail;

    /* ready queue: bounded but generous (worker nodes are scheduled <=1 each) */
    size_t worker_nodes = 0;
    for (size_t i = 0; i < f->n_nodes; i++)
        if (f->nodes[i].n_in > 0) worker_nodes++;
    f->ready = nqueue_new(worker_nodes > 0 ? worker_nodes * 2 : 16);
    if (!f->ready) goto fail;
    f->alive = (int)worker_nodes;
    if (worker_nodes == 0) nqueue_close(f->ready);

    free(out_cursor);
    free(fanin); free(fanout);
    free(nv.v);  free(ev.v);
    return f;

fail:
    free(out_cursor);
    free(fanin); free(fanout);
    free(nv.v);  free(ev.v);
    if (f) flow_free(f);
    return NULL;
}

/* ============================================================
 *  flow_start / stop / join / free
 * ============================================================ */

int flow_start(Flow *f) {
    size_t workers_started = 0;
    for (size_t i = 0; i < f->n_workers; i++) {
        if (pthread_create(&f->workers[i], NULL, worker_main, f) != 0) goto fail;
        workers_started++;
    }
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        if (n->n_in == 0 && n->n_out > 0) {
            if (pthread_create(&n->src_thread, NULL, source_main, n) != 0) goto fail;
            n->has_src_thread = 1;
        }
    }
    f->started = 1;
    return 0;

fail:
    /* Wake every thread, join everything that did start, and reset workers
     * to a known-joined state. Leaves Flow in the same shape it had before
     * flow_start so flow_free() can clean up safely. */
    f->stop = 1;
    if (f->ready) nqueue_close(f->ready);
    for (size_t i = 0; i < f->n_pqueues; i++)
        if (f->pqueues[i]) pqueue_close(f->pqueues[i]);
    for (size_t i = 0; i < f->n_nodes; i++) {
        Node *n = &f->nodes[i];
        if (n->has_src_thread) {
            pthread_join(n->src_thread, NULL);
            n->has_src_thread = 0;
        }
    }
    for (size_t j = 0; j < workers_started; j++)
        pthread_join(f->workers[j], NULL);
    return -1;
}

void flow_stop(Flow *f) {
    if (!f) return;
    f->stop = 1;
    /* Per-node shutdown hooks first: lets IO nodes close their private
     * blocking primitives (mqtt inbox, http listen socket, ...) so the
     * source thread can return cleanly before we touch the runtime
     * queues below. */
    for (size_t i = 0; i < f->n_nodes; i++)
        if (f->nodes[i].on_stop) f->nodes[i].on_stop(&f->nodes[i]);
    /* Wake every thread blocked on a queue condvar. Closing the ready
     * queue lets workers waiting in nqueue_pop() return NULL; closing
     * each packet queue lets sources stuck in pqueue_push() (full queue)
     * and workers stuck in pqueue_pop() bail out. Without this a hard
     * Ctrl-C can hang flow_join() forever when a queue is saturated. */
    if (f->ready) nqueue_close(f->ready);
    for (size_t i = 0; i < f->n_pqueues; i++)
        if (f->pqueues[i]) pqueue_close(f->pqueues[i]);
}

void flow_join(Flow *f) {
    if (!f || !f->started) return;
    for (size_t i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].has_src_thread) {
            pthread_join(f->nodes[i].src_thread, NULL);
            f->nodes[i].has_src_thread = 0;
        }
    }
    for (size_t i = 0; i < f->n_workers; i++)
        pthread_join(f->workers[i], NULL);
    f->started = 0;   /* makes flow_join + flow_free safe in any order */
}

void flow_free(Flow *f) {
    if (!f) return;
    /* Defensive: if flow_start was called but flow_join wasn't, do it
     * now. Destroying a mutex held by a running worker would be UB. */
    if (f->started) {
        flow_stop(f);
        flow_join(f);
    }
    if (f->nodes) {
        for (size_t i = 0; i < f->n_nodes; i++) {
            Node *n = &f->nodes[i];
            if (n->ctx_free && n->ctx) n->ctx_free(n->ctx);
            free(n->in);
            free(n->out);
            pthread_mutex_destroy(&n->state_mtx);
        }
        free(f->nodes);
    }
    if (f->pqueues) {
        for (size_t i = 0; i < f->n_pqueues; i++)
            pqueue_free(f->pqueues[i]);
        free(f->pqueues);
    }
    if (f->ready)   nqueue_free(f->ready);
    if (f->workers) free(f->workers);
    pthread_mutex_destroy(&f->alive_mtx);
    free(f);
}
