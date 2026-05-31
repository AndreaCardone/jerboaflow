# jerboa

A minimal flow-based runtime in C11. Wire a directed graph of nodes in
a plain-text config; the runtime schedules them across a fixed worker
pool. ~4k LoC, two dozen built-in node types, BSD-2-Clause. Only libc
and pthreads in the default build; Lua/MQTT/GPIO are opt-in.

> Named after the [jerboa](https://en.wikipedia.org/wiki/Jerboa) — a
> tiny, lightweight desert rodent that hops surprisingly fast for its
> size.

> **Status: 0.1.0-beta** — usable on trusted networks (homelab, edge,
> sidecar, behind a reverse proxy). **No TLS, no auth, no secrets
> handling yet** — don't expose the HTTP ingress or point MQTT at a
> public broker. Config grammar may still change before 1.0.
> Linux x86_64 / aarch64 only. See [CHANGELOG.md](CHANGELOG.md).

```                                          
                                                                                     +-----------+
               +----------+      +--------+                                     +--->| file_sink |--->(log_file)
(uart fifo)--->| epoll_in |----->| filter |-------------------+                 |    +-----------+
               +----------+      +--------+                   |                 |    
                                                              |                 |                
                                                              |                 |                   
                                                              |                 |                 
                                                              v                 |
               +----------+      +----------+     +-------------------------+   |    +----------+
(adc 2 fifo)-->| epoll_in |--+-->| throttle |---->|  lua (join and jsonify) |---+--->| mqtt pub |
               +----------+  |   +----------+     +-------------------------+        +----------+
                             |                                ^
                             |   +-----------------------+    |                      +----------+
                             +-->| compare (> threshold) |----|--------------------->| gpio_out |--->(led,siren,beacon)
                                 +-----------------------+    |                      +----------+
                                                              |                                                       
               +----------+                                   |                      +-----------------+
(button)------>| gpio_in  |-----------------------------------+--------------------->| lua (arm logic) |
               +----------+                                                          +-----------------+
```

## Quick start

```sh
git clone <repo> jerboa
cd jerboa
make jerboa
./jerboa examples/01_hello.conf
```

`flow.conf` is read by default if you omit the path. `Ctrl-C` for a
clean shutdown.

## Why

Most flow-based runtimes (Node-RED, NiFi, StreamSets) are JVM/Node.js
heavyweights that assume an admin UI, a package manager, and a few
hundred MB of RAM. jerboa is the opposite: a single static binary you
can drop on a router, configured by a file you can `vim`, observable
through a Prometheus endpoint, fully auditable in an afternoon.

In one line vs. the usual suspects:

- **vs. Node-RED** — same dataflow model, no browser, no Node.js, ~50 MB lighter, no contrib ecosystem (yet).
- **vs. Benthos / Vector** — far smaller scope (no schemas, no SQL, no cloud sinks), but a few MB instead of tens, and you can read the entire source over coffee.
- **vs. `socat` / shell pipes** — same "glue bytes between things" instinct, but with a real DAG, backpressure, per-node metrics, and graceful shutdown.
- **vs. rolling your own in C** — exactly what jerboa is, except it's already written, tested, and valgrind-clean.

Useful for:
- Edge / embedded ingest pipelines (sensors, MQTT, file tailing)
- Webhook receivers + filters + persistence
- Glue between processes with deterministic latency
- "I want Node-RED's model but bytes-through-pipes in C"

Not useful for:
- Visual flow editor (there is none — `vim flow.conf`)
- Hot reload (restart the process)
- Browser dashboards, drag-and-drop, contrib node ecosystem

See [docs/overview.md](docs/overview.md) for the longer story.

## Build

```sh
make                      # core binary
make WITH_LUA=1           # adds the `lua` node (needs lua5.4)
make WITH_MQTT=1          # adds `mqtt sub`/`mqtt pub` (needs libmosquitto)
make WITH_GPIO=1          # adds `gpio_in`/`gpio_out` (needs libgpiod)
make test                 # 28 tests
make test-valgrind        # same, under valgrind, zero leaks
make test-asan            # ASan + UBSan
```

Built with `-Wall -Wextra -Werror`, full RELRO/PIE, stack-protector,
FORTIFY_SOURCE=2, stack-clash protection. Verified clean under valgrind,
ASan+UBSan, and TSan. CI runs the full matrix on every push
(see [`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

## Security model

jerboa has **no TLS, no auth, no secrets handling**. The HTTP ingress,
metrics endpoint, and MQTT client are designed for trusted networks
(homelab, LAN, behind a reverse proxy, sidecar on `127.0.0.1`).

A `flow.conf` file is **trusted input**: the `exec` node runs
`/bin/sh -c <command>` and `lua` runs arbitrary scripts (sandboxed,
but still expressive). Treat the config the same way you'd treat a
shell script — never source it from an untrusted user.

## A flow in five lines

```
src   file_source /var/log/app.log
keep  filter ERROR
out   fwriter /tmp/errors.log
src -> keep
keep -> out
```

## Built-in nodes

| name          | role   | summary                                          |
|---------------|--------|--------------------------------------------------|
| `generator`   | source | emit `text-N` every N ms                         |
| `file_source` | source | one packet per line of a file                    |
| `http_in`     | source | HTTP POST receiver (fire-and-forget, replies 204)|
| `epoll_in`    | source | epoll(7) on FIFO / char device, one packet per line |
| `mqtt sub`    | source | MQTT subscriber (opt-in)                         |
| `uppercase`   | worker | uppercase payload                                |
| `filter`      | worker | substring match                                  |
| `filter_re`   | worker | POSIX extended regex filter                      |
| `batch`       | worker | coalesce N packets                               |
| `split`       | worker | split on delimiter                               |
| `throttle`    | worker | rate-limit                                       |
| `count`       | worker | emit running count                               |
| `tee`         | worker | pass-through                                     |
| `lua`         | worker | sandboxed Lua 5.4 script + bundled `json` (opt-in) |
| `exec`        | worker | pipe packet through a child process              |
| `http_out`    | sink   | HTTP POST to a remote URL                        |
| `printer`     | sink   | print to stdout                                  |
| `fwriter`     | sink   | append to a file                                 |
| `file_sink`   | sink   | write packets to a file                          |
| `mqtt pub`    | sink   | MQTT publisher (opt-in)                          |
| `gpio_in`     | source | edge events on a Linux GPIO line, emits `"1"`/`"0"` (opt-in, libgpiod) |
| `gpio_out`    | sink   | drive a Linux GPIO line per packet (opt-in, libgpiod) |
| `null`        | sink   | drop                                             |

Full table with arguments: [docs/nodes.md](docs/nodes.md).

## Observability

```sh
./jerboa flow.conf --metrics-port 9090
curl http://127.0.0.1:9090/metrics
```

Prometheus text format. Per-node packets in/out, process() calls and
wallclock, per-edge depth + high-water + drops. Loopback-only by design;
see [docs/operations.md](docs/operations.md) for proxying.

## Examples

See [examples/](examples/). Twenty flows from hello-world to a 201-node
stress DAG:

- [01_hello.conf](examples/01_hello.conf) — minimal generator → printer
- [02_log_filter.conf](examples/02_log_filter.conf) — tail-style filter
- [03_fanout.conf](examples/03_fanout.conf), [04_join.conf](examples/04_join.conf), [06_diamond.conf](examples/06_diamond.conf) — graph shapes
- [05_batch.conf](examples/05_batch.conf), [07_throttle.conf](examples/07_throttle.conf) — backpressure primitives
- [08_lua.conf](examples/08_lua.conf) — scripted scoring
- [09_mqtt.conf](examples/09_mqtt.conf) — MQTT round-trip
- [10_stress_200.conf](examples/10_stress_200.conf) — 201 nodes, 9 layers
- [11_http_webhook.conf](examples/11_http_webhook.conf) — HTTP ingest with curl examples
- [12_gpio_blink.conf](examples/12_gpio_blink.conf) — drive a GPIO line
- [13_exec_pipe.conf](examples/13_exec_pipe.conf) — pipe packets through a child process
- [15_http_out.conf](examples/15_http_out.conf) — POST to a remote HTTP endpoint
- [16_mqtt_json.conf](examples/16_mqtt_json.conf) — MQTT JSON → Lua classify → action
- [17_mqtt_threefields.conf](examples/17_mqtt_threefields.conf) — JSON in, JSON out, three parallel per-field actions
- [18_epoll_in.conf](examples/18_epoll_in.conf) — watch a FIFO with epoll, one packet per line
- [19_logic.conf](examples/19_logic.conf) — route, drop, or relabel packets with the `logic` node
- [20_sensor_merge.conf](examples/20_sensor_merge.conf) — three sensor files joined and JSON-encoded in Lua, with threshold-based alarms

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Short version: read
[AGENTS.md](AGENTS.md), keep CI green, no new runtime dependencies
without discussion.

## Documentation

Everything lives in [docs/](docs/):

1. [overview.md](docs/overview.md) — what jerboa is and isn't
2. [build.md](docs/build.md) — compile flags, optional features
3. [config.md](docs/config.md) — `flow.conf` syntax
4. [nodes.md](docs/nodes.md) — every built-in node, with args
5. [architecture.md](docs/architecture.md) — runtime internals
6. [extending.md](docs/extending.md) — write your own node type in C
7. [metrics.md](docs/metrics.md) — the Prometheus endpoint
8. [operations.md](docs/operations.md) — signals, backpressure, pitfalls

The canonical API is the header [jerboa.h](jerboa.h).

## License

BSD 2-Clause. See [LICENSE](LICENSE).

Third-party components and their licenses are listed in [NOTICES.md](NOTICES.md).
