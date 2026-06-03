/* http_io.c -- see http_io.h. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE                 /* memmem(3) */

#include "http_io.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define ACCEPT_TICK_MS 200

int http_io_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

void http_io_set_timeouts(int fd, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void http_io_send_status(int fd, const char *status) {
    char buf[160];
    int  n = snprintf(buf, sizeof(buf),
        "HTTP/1.0 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        status);
    if (n > 0 && (size_t)n < sizeof(buf))
        (void)http_io_write_all(fd, buf, (size_t)n);
}

int http_io_read_headers(int fd, char *buf, int cap, int *total) {
    int n = 0;
    while (n < cap) {
        ssize_t r = recv(fd, buf + n, (size_t)(cap - n), 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        n += (int)r;
        for (int i = 3; i < n; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n' &&
                buf[i-1] == '\r' && buf[i]   == '\n') {
                *total = n;
                return i + 1;
            }
        }
    }
    return -2;
}

void http_io_serve(int lfd, atomic_int *stop,
                   http_io_handler handle, void *user) {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) { perror("http_io: epoll_create1"); return; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        perror("http_io: epoll_ctl");
        close(ep);
        return;
    }
    while (!*stop) {
        struct epoll_event out;
        int r = epoll_wait(ep, &out, 1, ACCEPT_TICK_MS);
        if (r <= 0) {
            if (r < 0 && errno != EINTR) break;
            continue;
        }
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        handle(cfd, user);
        close(cfd);
    }
    close(ep);
}

/* ---------- HTTP/1.0 client ---------- */

#define CLIENT_REQ_HDR 1024

int http_url_parse(const char *url, HttpUrl *out, const char *who) {
    static const char scheme[] = "http://";
    if (strncasecmp(url, scheme, sizeof(scheme) - 1) != 0) {
        fprintf(stderr, "jerboa: %s: url must start with http://\n", who);
        return -1;
    }
    const char *p = url + sizeof(scheme) - 1;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL;

    const char *host_end = colon ? colon : (slash ? slash : p + strlen(p));
    size_t hlen = (size_t)(host_end - p);
    if (hlen == 0 || hlen >= sizeof(out->host)) {
        fprintf(stderr, "jerboa: %s: bad host\n", who); return -1;
    }
    memcpy(out->host, p, hlen); out->host[hlen] = '\0';

    if (colon) {
        const char *pstart = colon + 1;
        const char *pend = slash ? slash : pstart + strlen(pstart);
        size_t plen = (size_t)(pend - pstart);
        if (plen == 0 || plen >= sizeof(out->port)) {
            fprintf(stderr, "jerboa: %s: bad port\n", who); return -1;
        }
        for (size_t i = 0; i < plen; i++) {
            if (pstart[i] < '0' || pstart[i] > '9') {
                fprintf(stderr, "jerboa: %s: non-numeric port\n", who);
                return -1;
            }
        }
        memcpy(out->port, pstart, plen); out->port[plen] = '\0';
    } else {
        memcpy(out->port, "80", 3);
    }

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= sizeof(out->path)) {
            fprintf(stderr, "jerboa: %s: path too long\n", who); return -1;
        }
        memcpy(out->path, slash, plen + 1);
    } else {
        out->path[0] = '/'; out->path[1] = '\0';
    }
    return 0;
}

static int client_dial(const HttpUrl *u, int timeout_s, const char *who) {
    struct addrinfo hints = {0}, *res = NULL, *ai;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(u->host, u->port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "jerboa: %s: resolve %s: %s\n",
                who, u->host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        http_io_set_timeouts(fd, timeout_s);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        fprintf(stderr, "jerboa: %s: connect %s:%s: %s\n",
                who, u->host, u->port, strerror(errno));
    return fd;
}

/* Parse "HTTP/1.x NNN ..." status line. Returns code or -1. */
static int parse_status_line(const char *line, size_t n) {
    if (n < 12) return -1;
    if (memcmp(line, "HTTP/", 5) != 0) return -1;
    const char *sp = memchr(line, ' ', n);
    if (!sp) return -1;
    const char *p = sp + 1;
    if (p + 3 > line + n) return -1;
    for (int i = 0; i < 3; i++)
        if (p[i] < '0' || p[i] > '9') return -1;
    return (p[0]-'0')*100 + (p[1]-'0')*10 + (p[2]-'0');
}

void http_response_free(HttpResponse *r) {
    if (!r) return;
    free(r->raw);
    r->raw = NULL; r->body = NULL;
    r->raw_len = r->body_len = 0;
    r->status = -1; r->truncated = 0;
}

int http_client_do(const HttpUrl *u,
                   const char *method, const char *ctype,
                   const void *body, size_t body_len,
                   size_t raw_cap, int timeout_s,
                   const char *who,
                   HttpResponse *out) {
    int fd = client_dial(u, timeout_s, who);
    if (fd < 0) return -1;

    char hdr[CLIENT_REQ_HDR];
    int hn = snprintf(hdr, sizeof(hdr),
        "%s %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, u->path, u->host, ctype, body_len);
    if (hn <= 0 || (size_t)hn >= sizeof(hdr)) {
        fprintf(stderr, "jerboa: %s: request header too large\n", who);
        close(fd); return -1;
    }

    /* After write all shutdown(fd, SHUT_WR) is not needed because
     * 1. We already wrote Connection: close in the header.
     * 2. Many modern servers close connection if the connection is
     *    half-closed by the client.
     */
    if (http_io_write_all(fd, hdr, (size_t)hn) < 0 ||
        (body_len && http_io_write_all(fd, body, body_len) < 0)) {
        fprintf(stderr, "jerboa: %s: send: %s\n", who, strerror(errno));
        close(fd); return -1;
    }

    char *buf = malloc(raw_cap);
    if (!buf) { close(fd); return -1; }
    size_t got = 0;
    int    truncated = 0;
    while (got < raw_cap) {
        ssize_t r = recv(fd, buf + got, raw_cap - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "jerboa: %s: recv: %s\n", who, strerror(errno));
            free(buf); close(fd); return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    if (got == raw_cap) truncated = 1;
    close(fd);

    int    status = -1;
    char  *bptr = NULL;
    size_t blen = 0;
    char  *crlf = memmem(buf, got, "\r\n", 2);
    if (crlf) status = parse_status_line(buf, (size_t)(crlf - buf));
    char  *sep  = memmem(buf, got, "\r\n\r\n", 4);
    if (sep) {
        bptr = sep + 4;
        blen = got - (size_t)(bptr - buf);
    }

    out->raw       = buf;
    out->raw_len   = got;
    out->status    = status;
    out->body      = bptr;
    out->body_len  = blen;
    out->truncated = truncated;
    return 0;
}
