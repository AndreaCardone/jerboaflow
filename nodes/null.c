/* nodes/null.c -- sink that drops every packet (the "/dev/null" of jerboa). */
#include "nodes.h"

static Packet *process(Node *self, size_t idx, Packet *in) {
    (void)self; (void)idx;
    packet_release(in);
    return NULL;
}

static int init(Node *n, const char *args) {
    (void)args;
    n->process = process;
    return 0;
}

const NodeType ndt_null = { "null", init };
