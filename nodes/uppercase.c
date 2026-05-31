/* nodes/transform.c -- uppercase the payload in place (or copy if shared). */
#include "nodes.h"
#include <ctype.h>

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)self; (void)idx;
    if (!in) return NULL;
    int rc = atomic_load_explicit(&in->refcount, memory_order_acquire);
    if (rc == 1) {
        char *p = in->data;
        for (size_t i = 0; i < in->len; i++)
            p[i] = (char)toupper((unsigned char)p[i]);
        return in;
    }
    Packet *out = packet_new(in->data, in->len);
    if (out) {
        char *p = out->data;
        for (size_t i = 0; i < out->len; i++)
            p[i] = (char)toupper((unsigned char)p[i]);
    }
    packet_release(in);
    return out;
}

static int init(Node *n, const char *args) {
    (void)args;
    n->process = process;
    return 0;
}

const NodeType ndt_uppercase = { "uppercase", init };
