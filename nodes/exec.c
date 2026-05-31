/* nodes/exec.c -- run a shell command per packet, optionally bounded.
 *
 * Config:
 *   <name> exec [-t <ms>] <shell-command...>
 *
 * For every input packet, fork() + exec("/bin/sh", "-c", <cmd>).
 * The packet payload is written to the child's stdin while the child's
 * stdout is concurrently read (up to MAX_OUT) into a fresh packet that
 * is emitted downstream. stderr is left attached to jerboa's stderr.
 *
 * With `-t <ms>` (0 < ms <= 3600000) the entire child lifetime is
 * bounded: on expiry the child is sent SIGTERM, given a 200ms grace
 * period, then SIGKILL'd. Any output collected up to that point is
 * still emitted; an error is logged.
 *
 * stdin and stdout are both driven through poll() with the remaining
 * deadline, so a child that fills its stdout pipe before draining its
 * stdin will not deadlock.
 *
 * SECURITY: the command runs under /bin/sh -c -- shell metacharacters
 * are interpreted. Treat the config file as trusted code.
 */

#define _POSIX_C_SOURCE 200809L

#include "nodes.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUT  (1u << 20)         /* 1 MiB cap on captured stdout */

typedef struct {
    char *cmd;          /* shell command line; never NULL after init */
    int   timeout_ms;   /* 0 = no timeout */
} Ctx;

/* ---------- helpers ---------- */

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static long ms_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long s  = (long)(now.tv_sec  - t0->tv_sec) * 1000L;
    long ns = (now.tv_nsec - t0->tv_nsec) / 1000000L;
    return s + ns;
}

static int buf_append(char **buf, size_t *cap, size_t *len,
                      const char *src, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return 0;
}

/* ---------- process ---------- */

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;

    int in_pipe[2]  = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        perror("exec: pipe");
        if (in_pipe[0]  >= 0) close(in_pipe[0]);
        if (in_pipe[1]  >= 0) close(in_pipe[1]);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        packet_release(in);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("exec: fork");
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        packet_release(in);
        return NULL;
    }

    if (pid == 0) {
        /* --- child --- */
        if (dup2(in_pipe[0],  STDIN_FILENO)  < 0) _exit(127);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", c->cmd, (char *)NULL);
        _exit(127);
    }

    /* --- parent --- */
    close(in_pipe[0]);
    close(out_pipe[1]);
    set_nonblock(in_pipe[1]);
    set_nonblock(out_pipe[0]);

    const unsigned char *wp = in->data;
    size_t               wn = in->len;
    if (wn == 0) { close(in_pipe[1]); in_pipe[1] = -1; }

    char  *buf = NULL;
    size_t cap = 0, len = 0;
    int    overflow_drain = 0;
    int    timed_out = 0;
    int    err = 0;
    int    stdout_open = 1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (stdout_open) {
        int rem = -1;
        if (c->timeout_ms > 0) {
            long el = ms_since(&t0);
            long r  = (long)c->timeout_ms - el;
            if (r <= 0) { timed_out = 1; break; }
            rem = (int)r;
        }

        struct pollfd pfd[2];
        int idx_in = -1, idx_out, nfd = 0;
        if (in_pipe[1] >= 0) {
            pfd[nfd] = (struct pollfd){ in_pipe[1], POLLOUT, 0 };
            idx_in = nfd++;
        }
        pfd[nfd] = (struct pollfd){ out_pipe[0], POLLIN, 0 };
        idx_out = nfd++;

        int pr = poll(pfd, (nfds_t)nfd, rem);
        if (pr < 0) {
            if (errno == EINTR) continue;
            err = 1; break;
        }
        if (pr == 0) { timed_out = 1; break; }

        /* writable: feed more stdin */
        if (idx_in >= 0 && pfd[idx_in].revents) {
            if (pfd[idx_in].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(in_pipe[1]); in_pipe[1] = -1;
            } else if (pfd[idx_in].revents & POLLOUT) {
                ssize_t w = write(in_pipe[1], wp, wn);
                if (w < 0) {
                    if (errno != EAGAIN && errno != EINTR) {
                        close(in_pipe[1]); in_pipe[1] = -1;
                    }
                } else {
                    wp += (size_t)w; wn -= (size_t)w;
                    if (wn == 0) { close(in_pipe[1]); in_pipe[1] = -1; }
                }
            }
        }

        /* readable: drain stdout */
        if (pfd[idx_out].revents) {
            char tmp[4096];
            ssize_t r = read(out_pipe[0], tmp, sizeof(tmp));
            if (r > 0) {
                if (!overflow_drain) {
                    if (len + (size_t)r > MAX_OUT) {
                        size_t can = MAX_OUT - len;
                        if (can && buf_append(&buf, &cap, &len, tmp, can) < 0) {
                            err = 1; break;
                        }
                        overflow_drain = 1;
                    } else if (buf_append(&buf, &cap, &len, tmp, (size_t)r) < 0) {
                        err = 1; break;
                    }
                }
                /* if overflow_drain, the bytes are simply discarded */
            } else if (r == 0) {
                stdout_open = 0;
            } else if (errno != EAGAIN && errno != EINTR) {
                stdout_open = 0;
            }
        }
    }

    if (in_pipe[1] >= 0) { close(in_pipe[1]); in_pipe[1] = -1; }
    close(out_pipe[0]);

    /* reap, killing on timeout */
    int status = 0;
    if (timed_out) {
        kill(pid, SIGTERM);
        struct timespec grace = { 0, 200 * 1000000L };
        nanosleep(&grace, NULL);
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        fprintf(stderr, "exec/%s: timed out after %d ms\n",
                self->name, c->timeout_ms);
    } else {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "exec/%s: child exited %d\n",
                    self->name, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "exec/%s: child killed by signal %d\n",
                    self->name, WTERMSIG(status));
    }

    packet_release(in);

    if (err || len == 0) { free(buf); return NULL; }
    Packet *p = packet_new(buf, len);
    free(buf);
    return p;
}

/* ---------- teardown ---------- */

static void ctx_free(void *vp) {
    Ctx *c = vp;
    free(c->cmd);
    free(c);
}

/* ---------- init ---------- */

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: exec needs a shell command\n");
        return -1;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;

    /* Optional leading `-t <ms>`. */
    while (*args == ' ' || *args == '\t') args++;
    if (args[0] == '-' && args[1] == 't' &&
        (args[2] == ' ' || args[2] == '\t')) {
        const char *p = args + 2;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p || v <= 0 || v > 3600000) {
            fprintf(stderr, "jerboa: exec: -t needs positive ms (<=3600000)\n");
            free(c); return -1;
        }
        c->timeout_ms = (int)v;
        args = end;
        while (*args == ' ' || *args == '\t') args++;
    }

    if (!*args) {
        fprintf(stderr, "jerboa: exec: missing command\n");
        free(c); return -1;
    }

    c->cmd = strdup(args);
    if (!c->cmd) { free(c); return -1; }

    n->process  = process;
    n->ctx      = c;
    n->ctx_free = ctx_free;
    return 0;
}

const NodeType ndt_exec = { "exec", init };
