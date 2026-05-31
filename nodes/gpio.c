/* nodes/gpio.c -- Linux GPIO source / sink via libgpiod.
 *
 * Config:
 *   <name> gpio_in  <chip> <line> <edge>            -- source
 *   <name> gpio_out <chip> <line> [initial]         -- sink
 *
 * <chip>    is "gpiochip0", "0", or "/dev/gpiochip0" (libgpiod lookup).
 * <line>    is the integer offset within the chip.
 * <edge>    is one of: rising | falling | both.
 * [initial] is 0 or 1, defaults to 0.
 *
 * `gpio_in` waits for line events with a 200ms timeout (so we notice
 * flow->stop) and emits a 1-byte payload "1" on a rising edge or "0"
 * on a falling edge.
 *
 * `gpio_out` reads the first non-whitespace byte of each incoming
 * packet: '1' / 'h' / 't' -> high, anything else -> low. The packet
 * is then released and nothing is forwarded.
 *
 * Build with WITH_GPIO=1. Requires libgpiod-dev. */

#include "nodes.h"

#include <ctype.h>
#include <errno.h>
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { DIR_IN, DIR_OUT } Dir;

typedef struct {
    Dir                dir;
    struct gpiod_chip *chip;
    struct gpiod_line *line;
} Ctx;

/* ---------- process: gpio_in ---------- */

static Packet *process_in(Node *self, size_t idx, Packet *in) {
    (void)idx; (void)in;
    Ctx *c = self->ctx;
    while (!self->flow->stop) {
        struct timespec to = { 0, 200 * 1000000L };     /* 200 ms */
        int r = gpiod_line_event_wait(c->line, &to);
        if (r == 0) continue;                            /* timeout */
        if (r < 0) {
            if (self->flow->stop) return NULL;
            fprintf(stderr, "gpio_in/%s: event_wait: %s\n",
                    self->name, strerror(errno));
            return NULL;
        }
        struct gpiod_line_event ev;
        if (gpiod_line_event_read(c->line, &ev) < 0) {
            fprintf(stderr, "gpio_in/%s: event_read: %s\n",
                    self->name, strerror(errno));
            continue;
        }
        const char *v =
            (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE) ? "1" : "0";
        Packet *p = packet_new(v, 1);
        if (p) return p;
    }
    return NULL;
}

/* ---------- process: gpio_out ---------- */

static Packet *process_out(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    const unsigned char *d = in->data;
    int v = 0;
    for (size_t i = 0; i < in->len; i++) {
        unsigned char ch = d[i];
        if (isspace(ch)) continue;
        v = (ch == '1' || ch == 'h' || ch == 'H' || ch == 't' || ch == 'T');
        break;
    }
    if (gpiod_line_set_value(c->line, v) < 0)
        fprintf(stderr, "gpio_out/%s: set_value: %s\n",
                self->name, strerror(errno));
    packet_release(in);
    return NULL;
}

/* ---------- teardown ---------- */

static void ctx_free(void *p) {
    Ctx *c = p;
    if (c->line) gpiod_line_release(c->line);
    if (c->chip) gpiod_chip_close(c->chip);
    free(c);
}

/* ---------- init helpers ---------- */

static struct gpiod_chip *open_chip(const char *s) {
    /* Accept "gpiochip0", "/dev/gpiochip0", or bare digits "0". */
    if (s[0] >= '0' && s[0] <= '9' && !strchr(s, '/')) {
        char buf[32];
        snprintf(buf, sizeof(buf), "gpiochip%s", s);
        return gpiod_chip_open_lookup(buf);
    }
    return gpiod_chip_open_lookup(s);
}

/* ---------- init: gpio_in ---------- */

static int init_in(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: gpio_in needs: <chip> <line> <edge>\n");
        return -1;
    }
    char chip_s[64] = {0};
    unsigned offset = 0;
    char edge_s[16] = {0};
    if (sscanf(args, "%63s %u %15s", chip_s, &offset, edge_s) != 3) {
        fprintf(stderr, "jerboa: gpio_in: expected `<chip> <line> rising|falling|both`\n");
        return -1;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->dir = DIR_IN;
    c->chip = open_chip(chip_s);
    if (!c->chip) {
        fprintf(stderr, "gpio_in: open chip '%s': %s\n", chip_s, strerror(errno));
        free(c); return -1;
    }
    c->line = gpiod_chip_get_line(c->chip, offset);
    if (!c->line) {
        fprintf(stderr, "gpio_in: get_line(%u): %s\n", offset, strerror(errno));
        gpiod_chip_close(c->chip); free(c); return -1;
    }
    int rc;
    if      (strcmp(edge_s, "rising")  == 0)
        rc = gpiod_line_request_rising_edge_events(c->line, "jerboa");
    else if (strcmp(edge_s, "falling") == 0)
        rc = gpiod_line_request_falling_edge_events(c->line, "jerboa");
    else if (strcmp(edge_s, "both")    == 0)
        rc = gpiod_line_request_both_edges_events(c->line, "jerboa");
    else {
        fprintf(stderr, "gpio_in: edge must be rising|falling|both\n");
        gpiod_chip_close(c->chip); free(c); return -1;
    }
    if (rc < 0) {
        fprintf(stderr, "gpio_in: request events: %s\n", strerror(errno));
        gpiod_chip_close(c->chip); free(c); return -1;
    }

    n->process         = process_in;
    n->src_interval_ms = 0;   /* we block inside process_in ourselves */
    n->ctx             = c;
    n->ctx_free        = ctx_free;
    return 0;
}

/* ---------- init: gpio_out ---------- */

static int init_out(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: gpio_out needs: <chip> <line> [initial]\n");
        return -1;
    }
    char chip_s[64] = {0};
    unsigned offset = 0;
    int initial = 0;
    int got = sscanf(args, "%63s %u %d", chip_s, &offset, &initial);
    if (got < 2) {
        fprintf(stderr, "jerboa: gpio_out: expected `<chip> <line> [initial]`\n");
        return -1;
    }
    if (initial != 0 && initial != 1) {
        fprintf(stderr, "jerboa: gpio_out: initial must be 0 or 1\n");
        return -1;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->dir = DIR_OUT;
    c->chip = open_chip(chip_s);
    if (!c->chip) {
        fprintf(stderr, "gpio_out: open chip '%s': %s\n", chip_s, strerror(errno));
        free(c); return -1;
    }
    c->line = gpiod_chip_get_line(c->chip, offset);
    if (!c->line) {
        fprintf(stderr, "gpio_out: get_line(%u): %s\n", offset, strerror(errno));
        gpiod_chip_close(c->chip); free(c); return -1;
    }
    if (gpiod_line_request_output(c->line, "jerboa", initial) < 0) {
        fprintf(stderr, "gpio_out: request output: %s\n", strerror(errno));
        gpiod_chip_close(c->chip); free(c); return -1;
    }

    n->process  = process_out;
    n->ctx      = c;
    n->ctx_free = ctx_free;
    return 0;
}

const NodeType ndt_gpio_in  = { "gpio_in",  init_in  };
const NodeType ndt_gpio_out = { "gpio_out", init_out };
