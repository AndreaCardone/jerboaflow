# Extending: writing a new node

A node type is a struct plus an `init` callback. Look at any file in
[`nodes/`](../nodes/) for a working template; the simplest is
[`nodes/tee.c`](../nodes/tee.c) (10 lines).

## The contract

```c
const NodeType ndt_mything = { "mything", init };
```

- The string is the type name as it appears in `flow.conf`.
- `init(Node *n, const char *args)` populates `n->process`,
  optionally `n->ctx`/`n->ctx_free`, optionally `n->on_stop` (see
  [Blocking IO and shutdown](#blocking-io-and-shutdown)), optionally
  `n->src_interval_ms` for sources. Returns 0 on success, -1 on
  failure (must clean up partial state on -1).

## `process` callback

```c
typedef Packet *(*node_fn)(Node *self, size_t in_idx, Packet *in);
```

| Param      | Notes |
|------------|-------|
| `self`     | The node. `self->ctx` is your private state. |
| `in_idx`   | Which input port fired (0..`n_in-1`). For sources, always 0. |
| `in`       | The input packet, **already retained for you** (one ref). NULL for sources. You own it: release it (or return it). |
| **return** | A packet to emit (runtime takes one ref and pushes to all outputs), or NULL to emit nothing. |

The runtime guarantees `process()` runs single-threaded per node, so
`self->ctx` needs no locks.

## Source vs. worker

If your node is a source (no inputs in the config), set
`n->src_interval_ms` in `init`:

- `> 0`: runtime calls `process(self, 0, NULL)` every N ms.
- `0`: runtime calls `process` in a tight loop. Use this when your
  `process` blocks on its own (e.g., `mqtt sub` blocks on a private
  inbox `PQueue`; see [`nodes/mqtt.c`](../nodes/mqtt.c)).

Returning NULL from a source's `process` **terminates** that source.

## Blocking IO and shutdown

If your `process` (or a thread your node owns) blocks on something the
runtime doesn't know about — a private `PQueue`, an `accept()` socket,
an `epoll` fd, a `read()` on a pipe — set `n->on_stop`:

```c
typedef void (*on_stop_fn)(Node *self);
```

`flow_stop()` calls every node's `on_stop` (if non-NULL) on the
shutdown thread, **before** closing the runtime queues. Use it to
wake your blocking primitive so `process` (or your IO thread) can
return promptly:

```c
static void on_stop(Node *self) {
    Ctx *c = self->ctx;
    pqueue_close(c->inbox);            /* wakes pqueue_pop */
    if (c->lfd >= 0) shutdown(c->lfd, SHUT_RDWR);  /* wakes accept */
}
```

Without an `on_stop`, your node will only see shutdown when it next
finishes whatever it was blocked on — which may be never (`accept`
on an idle socket, `pqueue_pop` on an empty inbox). Pure
worker/transform nodes don't need it: the runtime already wakes them
by closing the ready queue.

## Emitting more than one packet

```c
static Packet *process(Node *self, size_t idx, Packet *in) {
    // emit two extras directly:
    Packet *a = packet_new("a", 1);
    Packet *b = packet_new("b", 1);
    node_emit(self, a);
    node_emit(self, b);
    // and a third as the return value:
    return in;   // pass the input through
}
```

`node_emit` takes one ref from you and pushes to every output queue.

## Registering

Add the symbol to the table in [`nodes/builtins.c`](../nodes/builtins.c):

```c
extern const NodeType ndt_mything;

static const NodeType *builtins[] = {
    // ...
    &ndt_mything,
    NULL,
};
```

And add the source file to `NODE_SRC` in the [`Makefile`](../Makefile)
(or follow the `WITH_LUA` / `WITH_MQTT` pattern for an optional node).

## Memory rules

- `packet_new(src, len)` returns refcount=1 and copies `len` bytes.
- `packet_retain(p)` bumps the count; `packet_release(p)` decrements
  and frees at 0. `packet_release(NULL)` is a no-op.
- If you receive `in` and don't return it (or pass it to `node_emit`),
  you **must** release it. Forgetting is the #1 source of leaks.

## Compile-time hygiene

- No `strcpy`/`strcat`/`sprintf`/`gets`.
- Bounds-check everything that touches user-supplied bytes.
- Compile under the project's flags (`-Wall -Wextra -Werror …`); a
  single warning fails the build.
- Run `make test-asan` and `make test-valgrind` after non-trivial
  changes.
