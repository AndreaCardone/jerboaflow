# jerboa documentation

A minimal flow-based runtime in C11. Executes a DAG of nodes across a fixed
worker pool. Configuration is a plain text file. ~2k LoC total.

## Reading order

1. [overview.md](overview.md) — what jerboa is, the data flow, what it is *not*
2. [build.md](build.md) — compile, optional features, sanitizers
3. [config.md](config.md) — `flow.conf` syntax
4. [nodes.md](nodes.md) — every built-in node and its arguments
5. [architecture.md](architecture.md) — runtime internals: scheduler, queues, shutdown
6. [extending.md](extending.md) — writing a new node type in C
7. [metrics.md](metrics.md) — Prometheus endpoint
8. [operations.md](operations.md) — running in production, signals, common pitfalls

The header [../jerboa.h](../jerboa.h) is the canonical public API.
