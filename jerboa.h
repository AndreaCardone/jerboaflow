/* jerboa.h -- minimal flow-based runtime. C11 + pthreads + <stdatomic.h>. */
#ifndef JERBOA_H
#define JERBOA_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* ---------- Packet (refcounted) ---------- */

typedef struct Packet {
    void       *data;       /* points just past the struct (single allocation) */
    size_t      len;
    atomic_int  refcount;   /* touch only via stdatomic.h */
} Packet;

Packet *packet_new(const void *src, size_t len);
void    packet_retain(Packet *p);
void    packet_release(Packet *p);              /* NULL-safe */

/* ---------- Bounded MPMC packet queue ---------- */

struct Node;

typedef struct PQueue {
    Packet        **buf;
    size_t          cap;        /* power of two */
    size_t          mask;
    size_t          head, tail, count;
    int             closed;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    struct Node    *consumer;   /* node that reads this queue; NULL if none */

    /* metrics (updated under mtx, read either under mtx or via atomic load) */
    uint64_t        m_pushed;
    uint64_t        m_dropped;
    size_t          m_highwater;
} PQueue;

PQueue *pqueue_new(size_t cap);
void    pqueue_free(PQueue *q);
int     pqueue_push(PQueue *q, Packet *p);          /* blocks; -1 if closed */
Packet *pqueue_pop(PQueue *q);                      /* blocks; NULL on close+empty */
int     pqueue_trypop(PQueue *q, Packet **out);     /* 0 ok, -1 empty */
size_t  pqueue_len(PQueue *q);
int     pqueue_done(PQueue *q);                     /* closed && empty */
void    pqueue_close(PQueue *q);

/* ---------- Node ---------- */

struct Flow;
struct Node;

/* in_idx: which input port fired (0..n_in-1). Sources: idx=0, in=NULL.
 * Ownership: callback owns the input ref (release if not returned).
 *            runtime takes one ref on the returned packet (may be `in`). */
typedef Packet *(*node_fn)(struct Node *self, size_t in_idx, Packet *in);

typedef struct Node {
    char            name[32];
    char            type[16];   /* node type name (for metrics labels) */
    node_fn         process;

    PQueue        **in;
    size_t          n_in;
    PQueue        **out;
    size_t          n_out;

    void           *ctx;
    void          (*ctx_free)(void *);

    /* Optional shutdown hook. Called once from flow_stop() before any
     * queues are closed, on the thread invoking flow_stop. Used by IO
     * nodes that block on something the runtime doesn't know about
     * (private inbox, socket accept, ...) to unblock themselves so
     * their process()/source thread can return promptly. NULL if
     * unused -- the runtime skips it. */
    void          (*on_stop)(struct Node *self);

    /* scheduler state (workers only) */
    pthread_mutex_t state_mtx;
    int             scheduled;
    int             running;
    int             needs_rescan;  /* push arrived while running */
    int             terminated;
    size_t          rr_cursor;

    /* source-only */
    pthread_t       src_thread;
    int             has_src_thread;
    unsigned        src_interval_ms;

    struct Flow    *flow;

    /* Metrics. Each node's process() runs on at most one thread at a time
     * (scheduler invariant), so the writer is single-threaded; the metrics
     * HTTP thread reads with atomic_load_explicit. Relaxed ordering is enough. */
    _Atomic uint64_t m_pkts_in;
    _Atomic uint64_t m_pkts_out;
    _Atomic uint64_t m_process_calls;
    _Atomic uint64_t m_process_ns;
    char             _m_pad[32];    /* pad to keep next node off this line */
} Node;

/* ---------- Node ready-queue (the worker pool's scheduler queue) ---------- */

typedef struct NQueue {
    Node          **buf;
    size_t          cap, mask, head, tail, count;
    int             closed;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} NQueue;

NQueue *nqueue_new(size_t cap);
void    nqueue_free(NQueue *q);
int     nqueue_push(NQueue *q, Node *n);
Node   *nqueue_pop(NQueue *q);
void    nqueue_close(NQueue *q);

/* ---------- Flow ---------- */

typedef struct Flow {
    Node           *nodes;
    size_t          n_nodes;
    PQueue        **pqueues;        /* every edge queue, owned */
    size_t          n_pqueues;

    NQueue         *ready;
    pthread_t      *workers;
    size_t          n_workers;

    /* number of worker (non-source) nodes still alive; ready queue closes at 0 */
    int             alive;
    pthread_mutex_t alive_mtx;

    atomic_int      stop;
    int             started;        /* 1 once flow_start spawned threads */
} Flow;

Flow *flow_load(const char *path, size_t n_workers);  /* 0 -> nproc */
int   flow_start(Flow *f);
void  flow_stop(Flow *f);
void  flow_join(Flow *f);
void  flow_free(Flow *f);

void  flow_schedule(Flow *f, Node *n);  /* idempotent; no-op for sources */

/* Push a packet to all outputs of `self`. Useful when a process() callback
 * wants to emit more than one packet per input. Takes ownership of one ref. */
void  node_emit(Node *self, Packet *p);

/* ---------- Node type registry ---------- */

typedef struct NodeType {
    const char *name;
    /* Initialize node fields: process, ctx, ctx_free, src_interval_ms.
     * Return 0 on success, -1 on failure. */
    int       (*init)(Node *n, const char *args);
} NodeType;

int              node_register(const NodeType *t);   /* idempotent by name */
const NodeType  *node_lookup  (const char *name);

/* ---------- Metrics HTTP endpoint (Prometheus text format) ----------
 *
 * Always-on counters live on Node and PQueue (see structs above). The
 * scrape endpoint is opt-in: a single listener thread on 127.0.0.1:port.
 * Both calls are no-ops if port <= 0 or already started/stopped. */
int  metrics_start(Flow *f, int port);
void metrics_stop(void);

#endif /* JERBOA_H */
