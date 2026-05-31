# Overview

jerboa runs a directed graph of **nodes** connected by **queues**.
Each node has a `process(packet)` callback that runs on a worker thread.
The runtime guarantees that **at most one worker runs a given node at a
time**, so node state needs no internal locking.

```
       generator        <- source: spawns its own thread
           |
           v
       uppercase        <- worker node: scheduled on the pool
           |
           v
        filter
           |
           v
       printer          <- sink: just a worker with no outputs
```

## The four types

| Type      | Definition (`jerboa.h`)        | Owns memory? |
|-----------|--------------------------------|--------------|
| `Packet`  | refcounted byte buffer         | yes (one alloc per packet, payload follows the struct) |
| `PQueue`  | bounded MPMC ring of `Packet*` | yes (power-of-two cap; mutex + 2 condvars) |
| `Node`    | name, type, process fn, ctx, in/out queues, metrics | borrows queues |
| `Flow`    | owns all `Node` and `PQueue`, worker pool, ready queue | owns everything |

A packet's lifecycle: `packet_new` (refcount=1) → pushed to N output
queues (refcount becomes N) → each consumer's worker pops it, calls
`process()`, eventually `packet_release` (refcount-1). At 0, the single
allocation is freed.

## Execution model

- One **source thread per source node** (a node with 0 inputs). It loops
  calling `process()` and pushes results downstream.
- A fixed **worker pool** (default = `nproc`) consumes from a single
  **ready queue** of nodes that have packets waiting. When a worker pops
  a node, it runs `process()` on the next ready input, then re-checks for
  more work on that node before releasing it.
- The scheduler invariant — *one worker per node at a time* — means each
  node's `ctx` is single-writer. No locks inside node code.

## What jerboa is not

- **Not a stream processor framework.** No windowing, no joins-by-key,
  no exactly-once semantics. Backpressure is "block on full queue".
- **Not distributed.** Single process, single host.
- **Not dynamic.** Flow is loaded once from a file; no hot reload.
- **Not a message broker.** Packets are in-memory only.

## When to use it

Embedded boxes that need to glue together a handful of I/O sources, a
few stateless transformations, and a few sinks (printer / file / MQTT
publish / null), with built-in observability and a hard upper bound on
memory use (queue capacity × packet size × edge count).
