# Config file syntax

Plain text, line-based, line-comments with `#`, blank lines ignored.
Two kinds of lines.

## 1. Node declarations

```
<name>   <type>   [args...]
```

- `name`: 1..31 chars, must be unique within the file.
- `type`: a registered node type (see [nodes.md](nodes.md)).
- `args`: free-form, consumed by the node's `init`; trimmed of leading/trailing space.

Examples:

```
gen   generator   500 100 hello
flt   filter      ERROR
out   printer     log
```

## 2. Edges

```
<src> -> <dst>[:<port>] [, <dst>[:<port>] ...]
```

- A bare `dst` is shorthand for `dst:0`.
- The right-hand side is a comma-separated **fan-out list**; every listed
  destination gets the same packet (refcount is bumped accordingly).
- Each `(dst, port)` pair gets exactly one queue. Two edges pointing at
  `dst:0` from different sources is a **join**, not two queues.

Examples:

```
gen     -> flt              # gen output 0 to flt input 0
gen     -> a, b, c          # fan-out: same packet to three nodes
a       -> j:0              # join: a and b both write to j
b       -> j:1              # j sees fan-in on two input ports
```

## Validation

`flow_load` rejects, with a clear error, any of:

- Unknown node type
- Edge referencing an unknown node name
- Duplicate node name
- Truncated name or type (declared with > 31 / > 15 chars)
- An input port declared but never wired (e.g., node uses port 2 but
  nothing pushes to port 2)

If anything fails, no resources are leaked.

## Worker count

Set on the command line, not in the config:

```
./jerboa flow.conf 8           # 8 worker threads
./jerboa flow.conf             # default: nproc
```

## Per-edge queue capacity

Currently a compile-time constant: `EDGE_CAP = 64` in [`jerboa.c`](../jerboa.c).

## A complete example

```
# 02_log_filter.conf -- tail a file, keep ERROR lines, print them.
src   file_source  examples/access.log
keep  filter       ERROR
out   printer      err

src  -> keep
keep -> out
```

See [`examples/`](../examples/) for nine progressively richer examples and
the [10_stress_200.conf](../examples/10_stress_200.conf) 201-node DAG.
