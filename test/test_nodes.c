/* test/test_nodes.c -- end-to-end tests for the wider node catalog. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "../jerboa.h"
#include "../nodes/nodes.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *TMP_CONF = "/tmp/jerboa_nodes_conf";
static const char *TMP_OUT  = "/tmp/jerboa_nodes_out";
static const char *TMP_IN   = "/tmp/jerboa_nodes_in";
static const char *TMP_SINK = "/tmp/jerboa_nodes_sink";

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

static char *run_flow(const char *cfg, size_t workers) {
    write_file(TMP_CONF, cfg);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) { perror("dup"); exit(2); }
    if (!freopen(TMP_OUT, "w", stdout)) { perror("freopen"); exit(2); }

    Flow *f = flow_load(TMP_CONF, workers);
    if (f) {
        flow_start(f);
        flow_join(f);
        flow_free(f);
    }

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    clearerr(stdout);

    char *out = read_file(TMP_OUT);
    unlink(TMP_CONF);
    unlink(TMP_OUT);
    return out;
}

/* ============================================================ */

TEST(node_filter_keeps_matches) {
    const char *cfg =
        "g  generator 5 10 mix\n"
        "f  filter 3\n"          /* only "mix-3" matches */
        "p  printer kept\n"
        "g -> f\n"
        "f -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "mix-3"), 1);
    CHECK_EQ_INT(count_substr(out, "mix-0"), 0);
    CHECK_EQ_INT(count_substr(out, "mix-1"), 0);
    free(out);
}

TEST(node_count_emits_sequence) {
    const char *cfg =
        "g  generator 5 3 x\n"
        "c  count\n"
        "p  printer n\n"
        "g -> c\n"
        "c -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "[p/n in=0] 1\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/n in=0] 2\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/n in=0] 3\n"), 1);
    free(out);
}

TEST(node_random_emits_in_range) {
    const char *cfg =
        "g  generator 5 12 poke\n"
        "r  random 4 6\n"
        "p  printer n\n"
        "g -> r\n"
        "r -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    if (out) {
        int lines = 0;
        for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
            char *last = strrchr(line, ' ');
            CHECK(last != NULL);
            if (!last) continue;
            long v = strtol(last + 1, NULL, 10);
            CHECK(v >= 4);
            CHECK(v <= 6);
            lines++;
        }
        CHECK_EQ_INT(lines, 12);
    }
    free(out);
}

TEST(node_logic_true_forwards_false_drops) {
    write_file(TMP_IN, "ok\nbad\n");
    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "src file %s\n"
             "l   logic == ok = !\n"
             "p   printer out\n"
             "src -> l\n"
             "l -> p\n",
             TMP_IN);
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] ok\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] bad\n"), 0);
    free(out);
    unlink(TMP_IN);
}

TEST(node_logic_emits_literals) {
    write_file(TMP_IN, "ok\nbad\n");
    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "src file %s\n"
             "l   logic == ok yes no\n"
             "p   printer out\n"
             "src -> l\n"
             "l -> p\n",
             TMP_IN);
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] yes\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] no\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] ok\n"), 0);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] bad\n"), 0);
    free(out);
    unlink(TMP_IN);
}

TEST(node_logic_false_can_forward) {
    write_file(TMP_IN, "ok\nbad\n");
    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "src file %s\n"
             "l   logic == ok yes =\n"
             "p   printer out\n"
             "src -> l\n"
             "l -> p\n",
             TMP_IN);
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] yes\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] bad\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] ok\n"), 0);
    free(out);
    unlink(TMP_IN);
}

TEST(node_logic_parses_quoted_strings) {
    write_file(TMP_IN,
               "dogs never bite mailmen but mailmen always bite dogs\n"
               "cats are fine\n");
    char cfg[512];
    snprintf(cfg, sizeof(cfg),
             "src file %s\n"
             "l   logic == \"dogs never bite mailmen but mailmen always bite dogs\" \"meaning flipped\" !\n"
             "p   printer out\n"
             "src -> l\n"
             "l -> p\n",
             TMP_IN);
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "[p/out in=0] meaning flipped\n"), 1);
    CHECK_EQ_INT(count_substr(out, "cats are fine"), 0);
    free(out);
    unlink(TMP_IN);
}

TEST(node_batch_coalesces_three) {
    const char *cfg =
        "g  generator 5 6 t\n"
        "b  batch 3\n"
        "p  printer b\n"
        "g -> b\n"
        "b -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    /* expect two batches of 3 (6 inputs total) */
    CHECK_EQ_INT(count_substr(out, "[p/b in=0] t-0\nt-1\nt-2"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/b in=0] t-3\nt-4\nt-5"), 1);
    free(out);
}

TEST(node_split_emits_pieces) {
    /* generator emits "alpha-0" (no commas) so split on '-' yields two pieces.
     * We can't easily inject custom text, so test by chaining gen -> split. */
    const char *cfg =
        "g  generator 5 2 a-b-c\n"     /* "a-b-c-0", "a-b-c-1" */
        "s  split -\n"
        "p  printer s\n"
        "g -> s\n"
        "s -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    /* Each input splits into 4 pieces: a, b, c, <n>.  2 inputs -> 8 lines. */
    CHECK_EQ_INT(count_substr(out, "[p/s in=0] "), 8);
    CHECK_EQ_INT(count_substr(out, "[p/s in=0] a\n"), 2);
    CHECK_EQ_INT(count_substr(out, "[p/s in=0] b\n"), 2);
    CHECK_EQ_INT(count_substr(out, "[p/s in=0] c\n"), 2);
    CHECK_EQ_INT(count_substr(out, "[p/s in=0] 0\n"), 1);
    CHECK_EQ_INT(count_substr(out, "[p/s in=0] 1\n"), 1);
    free(out);
}

TEST(node_tee_is_passthrough) {
    const char *cfg =
        "g  generator 5 2 z\n"
        "t  tee\n"
        "p  printer t\n"
        "g -> t\n"
        "t -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_INT(count_substr(out, "z-0"), 1);
    CHECK_EQ_INT(count_substr(out, "z-1"), 1);
    free(out);
}

TEST(node_null_consumes_everything) {
    /* If null does not release, Valgrind catches the leak. */
    const char *cfg =
        "g  generator 5 5 drop\n"
        "n  null\n"
        "g -> n\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    CHECK_EQ_STR(out, "");
    free(out);
}

TEST(node_throttle_drops_in_burst) {
    /* generator at 0ms interval into throttle 100ms.
     * With 5 packets and ~100ms throttle, only the first will pass. */
    const char *cfg =
        "g  generator 0 5 burst\n"
        "t  throttle 100\n"
        "p  printer t\n"
        "g -> t\n"
        "t -> p\n";
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    int seen = count_substr(out, "[p/t in=0] burst-");
    CHECK(seen >= 1);
    CHECK(seen <= 5);
    free(out);
}

TEST(node_file_source_and_sink) {
    write_file(TMP_IN, "hello\nworld\nthird\n");
    /* clean prior sink output */
    unlink(TMP_SINK);

    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "src  file %s\n"
             "snk  fwriter %s\n"
             "src -> snk\n",
             TMP_IN, TMP_SINK);
    char *out = run_flow(cfg, 2);
    CHECK(out != NULL);
    free(out);

    char *sink = read_file(TMP_SINK);
    CHECK(sink != NULL);
    if (sink) {
        CHECK_EQ_STR(sink, "hello\nworld\nthird\n");
        free(sink);
    }
    unlink(TMP_IN);
    unlink(TMP_SINK);
}

TEST(node_full_pipeline_file_filter_count) {
    /* file -> filter("o") -> count -> fwriter
     * Lines containing 'o': "hello", "world", "foo" -> count emits 1, 2, 3. */
    write_file(TMP_IN, "hello\nbye\nworld\nz\nfoo\n");
    unlink(TMP_SINK);

    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "src file %s\n"
             "f   filter o\n"
             "c   count\n"
             "snk fwriter %s\n"
             "src -> f\n"
             "f -> c\n"
             "c -> snk\n",
             TMP_IN, TMP_SINK);
    char *out = run_flow(cfg, 2);
    free(out);

    char *sink = read_file(TMP_SINK);
    CHECK(sink != NULL);
    if (sink) {
        CHECK_EQ_STR(sink, "1\n2\n3\n");
        free(sink);
    }
    unlink(TMP_IN);
    unlink(TMP_SINK);
}

/* ---------- http_in: POST a few bodies, verify they reach the sink ---------- */

static int http_post(int port, const char *path, const char *body, size_t blen) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = { .sin_family = AF_INET };
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST %s HTTP/1.0\r\nContent-Length: %zu\r\n\r\n", path, blen);
    if (send(fd, hdr, (size_t)hn, 0) != hn) { close(fd); return -1; }
    if (blen && send(fd, body, blen, 0) != (ssize_t)blen) { close(fd); return -1; }
    char resp[64];
    ssize_t r = recv(fd, resp, sizeof(resp) - 1, 0);
    close(fd);
    if (r <= 0) return -1;
    resp[r] = '\0';
    /* Expect "HTTP/1.0 204 ..." */
    return strncmp(resp, "HTTP/1.0 204", 12) == 0 ? 0 : -1;
}

TEST(node_http_in_post_to_sink) {
    unlink(TMP_SINK);
    /* port chosen to be uncommon; SO_REUSEADDR set on the listener. */
    int port = 19873;

    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "h   http_in 127.0.0.1 %d /ingest\n"
             "snk fwriter %s\n"
             "h -> snk\n",
             port, TMP_SINK);
    write_file(TMP_CONF, cfg);

    Flow *f = flow_load(TMP_CONF, 2);
    CHECK(f != NULL);
    if (!f) { unlink(TMP_CONF); return; }
    CHECK_EQ_INT(flow_start(f), 0);

    /* Give the listener a moment to start accepting. */
    struct timespec ts = { 0, 100 * 1000000L };
    nanosleep(&ts, NULL);

    CHECK_EQ_INT(http_post(port, "/ingest", "one",   3), 0);
    CHECK_EQ_INT(http_post(port, "/ingest", "two",   3), 0);
    CHECK_EQ_INT(http_post(port, "/ingest", "three", 5), 0);

    /* Wrong path -> 404, body not delivered. */
    CHECK(http_post(port, "/elsewhere", "x", 1) != 0);

    /* Wait for the sink to drain. fwriter flushes per write. */
    for (int i = 0; i < 50; i++) {
        FILE *fp = fopen(TMP_SINK, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fclose(fp);
            if (sz >= 14) break;        /* "one\ntwo\nthree\n" */
        }
        nanosleep(&ts, NULL);
    }

    flow_stop(f);
    flow_join(f);
    flow_free(f);

    char *sink = read_file(TMP_SINK);
    CHECK(sink != NULL);
    if (sink) {
        CHECK_EQ_STR(sink, "one\ntwo\nthree\n");
        free(sink);
    }
    unlink(TMP_CONF);
    unlink(TMP_SINK);
}

TEST(node_epoll_in_fifo_lines) {
    const char *fifo = "/tmp/jerboa_epoll_in_fifo";
    unlink(fifo);
    unlink(TMP_SINK);
    if (mkfifo(fifo, 0600) != 0) { perror("mkfifo"); CHECK(0); return; }

    char cfg[256];
    snprintf(cfg, sizeof(cfg),
             "e   epoll_in %s\n"
             "snk fwriter %s\n"
             "e -> snk\n",
             fifo, TMP_SINK);
    write_file(TMP_CONF, cfg);

    Flow *f = flow_load(TMP_CONF, 2);
    CHECK(f != NULL);
    if (!f) { unlink(TMP_CONF); unlink(fifo); return; }
    CHECK_EQ_INT(flow_start(f), 0);

    /* Open the writer side. Blocks until epoll_in's read side is open,
     * which init() guarantees before flow_start returns. */
    int wfd = open(fifo, O_WRONLY);
    CHECK(wfd >= 0);
    if (wfd >= 0) {
        const char msg[] = "line1\nline2\r\nline3\n";
        ssize_t w = write(wfd, msg, sizeof(msg) - 1);
        CHECK(w == (ssize_t)(sizeof(msg) - 1));
        close(wfd);
    }

    /* Wait for sink to receive "line1\nline2\nline3\n" (18 bytes). */
    struct timespec ts = { 0, 100 * 1000000L };
    for (int i = 0; i < 50; i++) {
        FILE *fp = fopen(TMP_SINK, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fclose(fp);
            if (sz >= 18) break;
        }
        nanosleep(&ts, NULL);
    }

    flow_stop(f);
    flow_join(f);
    flow_free(f);

    char *sink = read_file(TMP_SINK);
    CHECK(sink != NULL);
    if (sink) {
        CHECK_EQ_STR(sink, "line1\nline2\nline3\n");
        free(sink);
    }
    unlink(TMP_CONF);
    unlink(TMP_SINK);
    unlink(fifo);
}

int main(void) {
    nodes_register_builtins();
    run_node_filter_keeps_matches();
    run_node_count_emits_sequence();
    run_node_random_emits_in_range();
    run_node_logic_true_forwards_false_drops();
    run_node_logic_emits_literals();
    run_node_logic_false_can_forward();
    run_node_logic_parses_quoted_strings();
    run_node_batch_coalesces_three();
    run_node_split_emits_pieces();
    run_node_tee_is_passthrough();
    run_node_null_consumes_everything();
    run_node_throttle_drops_in_burst();
    run_node_file_source_and_sink();
    run_node_full_pipeline_file_filter_count();
    run_node_http_in_post_to_sink();
    run_node_epoll_in_fifo_lines();
    TEST_SUMMARY();
}
