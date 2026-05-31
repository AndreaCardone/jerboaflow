/* test/test_flow.c -- end-to-end flow scenarios.
 *
 * Strategy: write a small flow.conf to a temp file, freopen stdout to
 * another temp file, run flow_load + flow_start + flow_join + flow_free,
 * restore stdout, then read+parse the captured output. */
#include "test.h"
#include "../jerboa.h"
#include "../nodes/nodes.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static const char *TMP_CONF = "/tmp/jerboa_test_flow.conf";
static const char *TMP_OUT  = "/tmp/jerboa_test_out.txt";

static void write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) { perror("fopen"); exit(2); }
    fputs(content, fp);
    fclose(fp);
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) n++;
    return n;
}

/* Save real stdout, redirect to TMP_OUT, run the flow, restore stdout.
 * Returns the captured output (caller frees). */
static char *run_flow(const char *cfg, size_t workers) {
    write_file(TMP_CONF, cfg);

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { perror("dup"); exit(2); }
    FILE *redir = freopen(TMP_OUT, "w", stdout);
    if (!redir) { perror("freopen"); exit(2); }

    Flow *f = flow_load(TMP_CONF, workers);
    if (f) {
        flow_start(f);
        flow_join(f);
        flow_free(f);
    }

    fflush(stdout);
    /* restore real stdout */
    dup2(saved, STDOUT_FILENO);
    close(saved);
    clearerr(stdout);

    char *out = read_file(TMP_OUT);
    unlink(TMP_CONF);
    unlink(TMP_OUT);
    return out;
}

/* ============================================================
 *  Tests
 * ============================================================ */

TEST(flow_single_chain) {
    /* gen -> upper -> printer; emit 3 packets and shut down. */
    const char *cfg =
        "g  generator 10 3 hello\n"
        "u  uppercase\n"
        "p  printer out\n"
        "g -> u\n"
        "u -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "HELLO-0"), 1);
    CHECK_EQ_INT(count_substr(out, "HELLO-1"), 1);
    CHECK_EQ_INT(count_substr(out, "HELLO-2"), 1);
    CHECK_EQ_INT(count_substr(out, "HELLO-3"), 0);
    free(out);
}

TEST(flow_fanout_1_to_N) {
    /* gen -> p1, p2 ; each printer must see every packet exactly once. */
    const char *cfg =
        "g  generator 10 5 foo\n"
        "p1 printer left\n"
        "p2 printer right\n"
        "g -> p1, p2\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "[p1/left in=0] foo-"),  5);
    CHECK_EQ_INT(count_substr(out, "[p2/right in=0] foo-"), 5);
    free(out);
}

TEST(flow_multi_input_join) {
    /* gen1 -> j:0  ;  gen2 -> j:1  ;  j -> p
     * Both streams must reach the printer; the printer logs which input
     * each packet came from (in=0 for gen1 path, in=1 for gen2 path).
     * After transform, that index information is lost — the join is on
     * the transform itself, which sees both ports. We verify counts. */
    const char *cfg =
        "g1 generator 5 4 aaa\n"
        "g2 generator 7 4 bbb\n"
        "j  uppercase\n"
        "p  printer joined\n"
        "g1 -> j:0\n"
        "g2 -> j:1\n"
        "j  -> p\n";
    char *out = run_flow(cfg, 4);
    CHECK(out != NULL);
    /* 4 packets from each source, uppercased */
    CHECK_EQ_INT(count_substr(out, "AAA-"), 4);
    CHECK_EQ_INT(count_substr(out, "BBB-"), 4);
    /* and 8 total printer lines */
    CHECK_EQ_INT(count_substr(out, "[p/joined"), 8);
    free(out);
}

TEST(flow_fanout_then_join_diamond) {
    /* Diamond:
     *      g --> u1 --\
     *       \-> u2 --> p
     * Each generated packet is fanned out to two transforms; both feed
     * the same printer on different input ports. Printer should see 2N
     * packets total for N generated. */
    const char *cfg =
        "g  generator 5 3 diam\n"
        "u1 uppercase\n"
        "u2 uppercase\n"
        "p  printer d\n"
        "g  -> u1, u2\n"
        "u1 -> p:0\n"
        "u2 -> p:1\n";
    char *out = run_flow(cfg, 4);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "DIAM-"), 6);    /* 3 packets * 2 paths */
    CHECK_EQ_INT(count_substr(out, "in=0"),  3);
    CHECK_EQ_INT(count_substr(out, "in=1"),  3);
    free(out);
}

TEST(flow_zero_packets_clean_shutdown) {
    /* count=1 with very short interval: still must shut down cleanly. */
    const char *cfg =
        "g generator 1 1 z\n"
        "p printer x\n"
        "g -> p\n";
    char *out = run_flow(cfg, 1);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "z-0"), 1);
    free(out);
}

int main(void) {
    nodes_register_builtins();
    run_flow_single_chain();
    run_flow_fanout_1_to_N();
    run_flow_multi_input_join();
    run_flow_fanout_then_join_diamond();
    run_flow_zero_packets_clean_shutdown();
    TEST_SUMMARY();
}
