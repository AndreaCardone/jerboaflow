/* test/test_packet.c -- Packet refcount semantics. */
#include "test.h"
#include "../jerboa.h"

#include <pthread.h>
#include <stdint.h>

TEST(packet_new_copies_payload) {
    const char *src = "hello";
    Packet *p = packet_new(src, 5);
    CHECK(p != NULL);
    CHECK_EQ_INT(p->len, 5);
    CHECK(p->data != src);                       /* must be a copy */
    CHECK(memcmp(p->data, "hello", 5) == 0);
    packet_release(p);
}

TEST(packet_new_zero_len) {
    Packet *p = packet_new(NULL, 0);
    CHECK(p != NULL);
    CHECK_EQ_INT(p->len, 0);
    packet_release(p);
}

TEST(packet_retain_release_balance) {
    Packet *p = packet_new("x", 1);
    packet_retain(p);
    packet_retain(p);
    /* refcount = 3; need 3 releases to free. We can't observe the free
     * directly, but ASan/Valgrind will catch a leak or double-free. */
    packet_release(p);
    packet_release(p);
    packet_release(p);
}

TEST(packet_release_null_is_safe) {
    packet_release(NULL);   /* must not crash */
}

/* ---- concurrent retain/release: every thread does N retain+release.
 * If the refcount logic is broken (non-atomic), we'd leak or double-free.
 * The packet starts with refcount=1 and we release it last from main.    */

#define N_THREADS 8
#define N_OPS     10000

static void *churn(void *arg) {
    Packet *p = arg;
    for (int i = 0; i < N_OPS; i++) {
        packet_retain(p);
        packet_release(p);
    }
    return NULL;
}

TEST(packet_refcount_is_atomic) {
    Packet *p = packet_new("z", 1);
    pthread_t th[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        pthread_create(&th[i], NULL, churn, p);
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(th[i], NULL);
    packet_release(p);   /* final release frees */
}

int main(void) {
    run_packet_new_copies_payload();
    run_packet_new_zero_len();
    run_packet_retain_release_balance();
    run_packet_release_null_is_safe();
    run_packet_refcount_is_atomic();
    TEST_SUMMARY();
}
