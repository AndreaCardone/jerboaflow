# Metrics

Opt-in Prometheus text-format endpoint (version 0.0.4) over plain HTTP.

## Enable

```sh
./jerboa flow.conf --metrics-port 9090
```

Binds to **`127.0.0.1` only**. To reach it from another host, run an SSH
tunnel (`ssh -L 9090:127.0.0.1:9090 host`) or put a reverse proxy in
front. Loopback-only is deliberate: jerboa speaks plain HTTP with no
auth.

## Scrape

```sh
curl http://127.0.0.1:9090/metrics
```

## Exposed series

| Series                                  | Type    | Labels                  | Meaning |
|-----------------------------------------|---------|-------------------------|---------|
| `jerboa_node_packets_in_total`          | counter | `node`, `type`          | Packets the node's `process()` consumed |
| `jerboa_node_packets_out_total`         | counter | `node`, `type`          | Packets the node emitted (incl. fan-out) |
| `jerboa_node_process_calls_total`       | counter | `node`, `type`          | Number of `process()` invocations |
| `jerboa_node_process_seconds_total`     | counter | `node`, `type`          | Wallclock time spent inside `process()` |
| `jerboa_queue_depth`                    | gauge   | `node`, `port`          | Current packets in the queue |
| `jerboa_queue_high_water_packets`       | gauge   | `node`, `port`          | Max depth observed since start |
| `jerboa_queue_pushed_total`             | counter | `node`, `port`          | Successful pushes |
| `jerboa_queue_dropped_total`            | counter | `node`, `port`          | Pushes rejected (queue closed) |

`node` is the consumer name for queue metrics. `port` is its input port.

## Useful queries

```promql
# per-node throughput (packets/s)
rate(jerboa_node_packets_out_total[1m])

# avg time per process() call, ms
1000 * rate(jerboa_node_process_seconds_total[1m])
       / rate(jerboa_node_process_calls_total[1m])

# saturation: queues running >75% full
jerboa_queue_high_water_packets / 64 > 0.75
```

The `64` denominator is `EDGE_CAP` from `jerboa.c`.

## Resource guarantees

The endpoint enforces:
- 5 s receive + send timeout on every accepted connection (slowloris-proof).
- 1 KB max request; missing CRLF in the first read → HTTP 400.
- One connection at a time (sufficient for scrape; rendering is microseconds).
- Allocation failures during rendering → HTTP 500 with `Content-Length: 0`
  (no truncated responses with a lying header).

## Shutdown

`metrics_stop()` calls `shutdown(SHUT_RDWR)` on the listening socket
before joining the thread, so a stuck `accept` returns immediately.
The endpoint always shuts down within the 200 ms scheduler tick.
