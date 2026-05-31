/* test/test_pqueue.c -- bounded MPMC packet queue. */
#include "test.h"
#include "../jerboa.h"

#include <pthread.h>

TEST(pqueue_basic_push_pop) {
    PQueue *q = pqueue_new(4);
    CHECK(q != NULL);
    Packet *p1 = packet_new("a", 1);
    Packet *p2 = packet_new("b", 1);
    CHECK_EQ_INT(pqueue_push(q, p1), 0);
    CHECK_EQ_INT(pqueue_push(q, p2), 0);
    CHECK_EQ_INT(pqueue_len(q), 2);

    Packet *r1 = pqueue_pop(q);
    Packet *r2 = pqueue_pop(q);
    CHECK(r1 == p1);
    CHECK(r2 == p2);
    CHECK_EQ_INT(pqueue_len(q), 0);

    packet_release(r1);
    packet_release(r2);
    pqueue_free(q);
}

TEST(pqueue_close_unblocks_pop) {
    PQueue *q = pqueue_new(2);
    pqueue_close(q);
    Packet *r = pqueue_pop(q);
    CHECK(r == NULL);
    CHECK(pqueue_done(q));
    pqueue_free(q);
}

TEST(pqueue_close_drains_pending) {
    PQueue *q = pqueue_new(2);
    Packet *p = packet_new("x", 1);
    pqueue_push(q, p);
    pqueue_close(q);
    Packet *r = pqueue_pop(q);
    CHECK(r == p);
    CHECK(pqueue_pop(q) == NULL);
    packet_release(r);
    pqueue_free(q);
}

TEST(pqueue_push_after_close_fails) {
    PQueue *q = pqueue_new(2);
    pqueue_close(q);
    Packet *p = packet_new("x", 1);
    CHECK_EQ_INT(pqueue_push(q, p), -1);
    packet_release(p);
    pqueue_free(q);
}

TEST(pqueue_trypop_empty) {
    PQueue *q = pqueue_new(2);
    Packet *r = NULL;
    CHECK_EQ_INT(pqueue_trypop(q, &r), -1);
    pqueue_free(q);
}

TEST(pqueue_free_releases_leftover) {
    /* Leak check job: pqueue_free must release any packets still inside.
     * Valgrind/ASan will catch a leak if this is broken. */
    PQueue *q = pqueue_new(8);
    for (int i = 0; i < 5; i++) pqueue_push(q, packet_new("y", 1));
    pqueue_free(q);
}

/* ---- producer / consumer stress ---- */

#define PROD_N 4
#define CONS_N 4
#define PER    2000

typedef struct {
    PQueue *q;
    int     made;
} Prod;

typedef struct {
    PQueue *q;
    int     consumed;
} Cons;

static void *producer(void *arg) {
    Prod *s = arg;
    for (int i = 0; i < PER; i++) {
        Packet *p = packet_new("p", 1);
        if (pqueue_push(s->q, p) == 0) s->made++;
        else packet_release(p);
    }
    return NULL;
}

static void *consumer(void *arg) {
    Cons *s = arg;
    for (;;) {
        Packet *p = pqueue_pop(s->q);
        if (!p) return NULL;
        s->consumed++;
        packet_release(p);
    }
}

TEST(pqueue_concurrent) {
    PQueue *q = pqueue_new(16);
    pthread_t pt[PROD_N], ct[CONS_N];
    Prod ps[PROD_N] = {0};
    Cons cs[CONS_N] = {0};

    for (int i = 0; i < CONS_N; i++) { cs[i].q = q; pthread_create(&ct[i], NULL, consumer, &cs[i]); }
    for (int i = 0; i < PROD_N; i++) { ps[i].q = q; pthread_create(&pt[i], NULL, producer, &ps[i]); }

    for (int i = 0; i < PROD_N; i++) pthread_join(pt[i], NULL);
    pqueue_close(q);
    for (int i = 0; i < CONS_N; i++) pthread_join(ct[i], NULL);

    int made = 0, got = 0;
    for (int i = 0; i < PROD_N; i++) made += ps[i].made;
    for (int i = 0; i < CONS_N; i++) got  += cs[i].consumed;
    CHECK_EQ_INT(made, PROD_N * PER);
    CHECK_EQ_INT(got,  PROD_N * PER);

    pqueue_free(q);
}

int main(void) {
    run_pqueue_basic_push_pop();
    run_pqueue_close_unblocks_pop();
    run_pqueue_close_drains_pending();
    run_pqueue_push_after_close_fails();
    run_pqueue_trypop_empty();
    run_pqueue_free_releases_leftover();
    run_pqueue_concurrent();
    TEST_SUMMARY();
}
