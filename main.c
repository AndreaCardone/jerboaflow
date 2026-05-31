/* main.c -- jerboa driver. */
#define _POSIX_C_SOURCE 200809L

#include "jerboa.h"
#include "nodes/nodes.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Self-pipe for async-signal-safe shutdown. The handler only calls write(2)
 * on a pipe fd -- write() is async-signal-safe; pthread_mutex_lock and
 * cond_broadcast (which flow_stop() uses) are not. The main thread polls
 * the read end and then calls flow_stop() from normal context. */
static int g_sigpipe[2] = { -1, -1 };

static void on_signal(int sig) {
    (void)sig;
    if (g_sigpipe[1] < 0) return;
    /* one byte, errors ignored on purpose (we just need to wake the poll) */
    char b = 1;
    ssize_t r = write(g_sigpipe[1], &b, 1);
    (void)r;
}

/* Watcher thread: joins the flow; when it returns naturally (bounded flow
 * finished on its own), nudge the pipe so main() unblocks. */
typedef struct { Flow *f; int wfd; } WatcherArg;

static void *watcher_main(void *p) {
    WatcherArg *a = p;
    flow_join(a->f);
    char b = 1;
    ssize_t r = write(a->wfd, &b, 1);
    (void)r;
    free(a);
    return NULL;
}

#ifndef JERBOA_VERSION
#define JERBOA_VERSION "0.1.0-beta"
#endif

int main(int argc, char **argv) {
    const char *cfg = "flow.conf";
    size_t workers = 0;  /* 0 = nproc */
    int    metrics_port = 0;
    int    pos = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--metrics-port") == 0 && i + 1 < argc) {
            metrics_port = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("jerboa %s\n", JERBOA_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [config] [workers] [--metrics-port N] [--version]\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [config] [workers] [--metrics-port N] [--version]\n", argv[0]);
            return 1;
        } else if (pos++ == 0) {
            cfg = argv[i];
        } else {
            long w = strtol(argv[i], NULL, 10);
            if (w > 0) workers = (size_t)w;
        }
    }

    nodes_register_builtins();

    Flow *f = flow_load(cfg, workers);
    if (!f) { fprintf(stderr, "jerboa: failed to load %s\n", cfg); return 1; }

    if (pipe(g_sigpipe) != 0) {
        perror("pipe");
        flow_free(f);
        return 1;
    }
    /* Non-blocking write so a signal flood can't ever block the handler. */
    int fl = fcntl(g_sigpipe[1], F_GETFL, 0);
    if (fl >= 0) fcntl(g_sigpipe[1], F_SETFL, fl | O_NONBLOCK);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* SIGPIPE on a closed metrics-client socket must not kill us. */
    signal(SIGPIPE, SIG_IGN);

    if (metrics_port > 0) metrics_start(f, metrics_port);

    if (flow_start(f) < 0) {
        fprintf(stderr, "jerboa: failed to start\n");
        metrics_stop();
        flow_free(f);
        close(g_sigpipe[0]); close(g_sigpipe[1]);
        return 1;
    }

    /* Watcher thread: joins flow, then signals us. */
    pthread_t watcher;
    int watcher_started = 0;
    WatcherArg *warg = malloc(sizeof(*warg));
    if (warg) {
        warg->f = f; warg->wfd = g_sigpipe[1];
        if (pthread_create(&watcher, NULL, watcher_main, warg) == 0)
            watcher_started = 1;
        else
            free(warg);
    }

    /* Block until something writes to the pipe (signal or watcher). */
    char buf[16];
    for (;;) {
        ssize_t n = read(g_sigpipe[0], buf, sizeof(buf));
        if (n > 0) break;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    flow_stop(f);
    if (watcher_started) pthread_join(watcher, NULL);
    else flow_join(f);   /* no watcher: join here ourselves */

    metrics_stop();
    flow_free(f);
    close(g_sigpipe[0]); close(g_sigpipe[1]);
    return 0;
}
