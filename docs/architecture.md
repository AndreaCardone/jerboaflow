# Architecture

The public API is in [`jerboa.h`](../jerboa.h) (177 lines); the
implementation is [`jerboa.c`](../jerboa.c) (~850 lines). Read those
two files first if anything below is ambiguous — they are the source
of truth.

## Memory model

### Packet

```c
typedef struct Packet {
    void       *data;       /* points just past the struct */
    size_t      len;
    atomic_int  refcount;
} Packet;
```

One `malloc` per packet, sized `sizeof(Packet) + len`. The payload
lives in the same allocation, immediately after the struct; `data`
just points into it. This halves allocator pressure versus a
header-plus-buffer pair and keeps the payload hot in the same cache
line as its length.

Refcounting uses `<stdatomic.h>`: relaxed for `packet_retain`,
acquire-release for `packet_release`. `packet_release(NULL)` is a no-op.

### Queues

Two bounded MPMC queues, both mutex + two condvars (`not_empty`,
`not_full`):

- `PQueue` carries `Packet*`. One per (consumer node, input port).
  Power-of-two capacity, `EDGE_CAP = 64`.
- `NQueue` carries `Node*`. One per `Flow`, the scheduler's *ready
  queue*.

Both expose a `close()` that wakes every waiter: producers blocked in
`push` return `-1`, consumers blocked in `pop` return `NULL` (or `-1`).
`pqueue_push(NULL)` is refused at the door — a `NULL` payload would
collide with the close-and-empty sentinel on the pop side.

Memory ceiling for the whole runtime is therefore bounded by
`edges × EDGE_CAP × max_packet_size`.

## Scheduler

The core invariant: **at most one worker runs a given node at a time.**
That guarantee is what lets every `process()` function be lock-free.

Per node, the scheduler tracks four flags under a `state_mtx`:

| flag           | meaning                                                |
|----------------|--------------------------------------------------------|
| `scheduled`    | sitting on the ready queue, not yet picked up          |
| `running`      | a worker is currently inside `process()` for this node |
| `needs_rescan` | a packet arrived while `running` — re-enqueue on exit  |
| `terminated`   | all inputs closed and drained                          |

Lifecycle of one packet:

1. A producer pushes into `consumer->in[port]` and calls
   `flow_schedule(consumer)`.
2. `flow_schedule` takes `state_mtx`. If the consumer is already
   `running`, it sets `needs_rescan = 1` and returns. Otherwise it
   sets `scheduled = 1` and pushes the node onto `ready`.
3. A worker pops the node, takes `state_mtx`, flips `scheduled → 0`
   and `running → 1`, releases the mutex.
4. The worker round-robins the node's input ports, popping one
   packet per pass and calling `process()`. Outputs go through
   `node_emit`, which is what feeds step 1 for downstream nodes.
5. When no input has anything ready, the worker re-takes `state_mtx`.
   If `needs_rescan` was set in the meantime, clear it and loop. Else
   clear `running` and pop the next node from `ready`.

This is the entire concurrency model. There are no per-node locks
exposed to node code; the scheduler is the serialization point.

## Termination

- A **source** node is done the moment its `process()` returns `NULL`
  — the source thread simply exits.
- A **worker** node is done when every input queue is closed *and*
  drained. The flow keeps a count `Flow.alive` of worker nodes still
  running; when it hits zero the ready queue is closed and the worker
  pool drains.

`flow_stop(f)` performs these four steps in order:

1. Set `f->stop = 1`.
2. Call each node's optional `on_stop()` hook. IO nodes that block
   inside something the runtime does not own (private inbox, `accept`,
   `epoll_wait`, libmosquitto loop, libgpiod event wait, …) use this
   hook to unblock themselves. Examples: `mqtt sub` closes its inbox
   `PQueue`; `http_in` shuts the listen socket and closes its inbox;
   `epoll_in` flips its stop flag and closes the inbox.
3. Close the ready `NQueue` (idle workers wake from `nqueue_pop`).
4. Close every `PQueue` (blocked producers return `-1`, blocked
   consumers return `NULL`).

`flow_join(f)` then joins every source thread and every worker
thread, and flips `started = 0`. `flow_free(f)` runs `stop + join`
first if necessary (destroying a held mutex is UB), then walks the
flow tearing down per-node `ctx_free`, mutexes, queues, the workers
array, and finally the `Flow` itself. Any packets left in any queue
at free time are released, not leaked.

## Signal handling

The signal handler in [`main.c`](../main.c) does the bare minimum the
async-signal-safe rules allow: it writes one byte to a self-pipe.
`pthread_mutex_lock` is **not** async-signal-safe, so calling
`flow_stop` directly from a handler deadlocks the moment a signal
arrives while any worker holds a `state_mtx`.

A small watcher thread joins the flow normally. Whichever event
happens first — natural flow exit or signal — writes to the pipe.
The main thread wakes from `read()` and invokes `flow_stop` +
`flow_join` from normal thread context.

## Shared HTTP/1.0 plumbing

Three nodes speak HTTP/1.0: `http_in` (server), `http_out` (client),
and the `/metrics` endpoint. Rather than duplicate the socket dance
three times, the shared bits live in [`http_io.h`](../http_io.h) /
[`http_io.c`](../http_io.c):

- **Server side**: `write_all`, `set_timeouts`, `send_status`,
  `read_headers`, and a tiny `serve(lfd, &stop, handler, user)`
  listener skeleton.
- **Client side**: `HttpUrl` + `http_url_parse`, plus
  `http_client_do` which dials, sends, drains, and parses the status
  line into an `HttpResponse`.

No external HTTP library is pulled in. Adding `libcurl`,
`libmicrohttpd`, or `cpp-httplib` would either drag a C++ runtime or
a heavy dependency tree in to replace a few hundred lines of
transparent C.

## Metrics

Each `Node` carries four counters: `m_pkts_in`, `m_pkts_out`,
`m_process_calls`, `m_process_ns`. They are `_Atomic uint64_t`
fields, padded to a 64-byte cache line to avoid false sharing between
neighbouring nodes. Each `PQueue` carries `m_pushed`, `m_dropped`,
and `m_highwater`, updated under the queue mutex.

The Prometheus scrape thread reads node counters with
`atomic_load_explicit(memory_order_relaxed)` and queue counters under
the queue mutex. The renderer never blocks the data plane.

Wire format and full counter list: [metrics.md](metrics.md).
