/* nodes/tee.c -- pure pass-through (useful with fan-out for debugging). */
#include "nodes.h"

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)self; (void)idx;
    return in;
}

static int init(Node *n, const char *args) {
    (void)args;
    n->process = process;
    return 0;
}

const NodeType ndt_tee = { "tee", init };
