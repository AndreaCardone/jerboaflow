# Operations

## Running

```sh
./jerboa <config> [workers] [--metrics-port N]
```

- `config`: path to a `flow.conf` (default `flow.conf` in cwd).
- `workers`: worker-pool size (default = `nproc`). Sources do not count
  against this.
- `--metrics-port`: enable the Prometheus endpoint on `127.0.0.1:N`.

Exit codes: `0` on clean shutdown, `1` on a load or start failure.

## Signals

| Signal     | Behavior |
|------------|----------|
| `SIGINT`   | Clean shutdown (close queues, join all threads, exit 0). |
| `SIGTERM`  | Same as SIGINT. |
| `SIGPIPE`  | Ignored (so a closed metrics-client socket cannot kill the process). |

The handler itself is async-signal-safe — it just writes one byte to a
self-pipe. The shutdown actually runs from the main thread. See
[architecture.md](architecture.md#signal-handling-in-mainc) for why.

Clean shutdown completes well under 1 s even on the 201-node stress
example.

## Backpressure

Per-edge queues are bounded (`EDGE_CAP = 64`). A full queue **blocks
the producer** in `pqueue_push`. This propagates upstream until a source
is throttled by its own `interval_ms` or until something gives.

Two implications:

1. A fast source feeding a slow sink will eventually pin both threads
   and stall — by design.
2. There is no built-in drop policy. If you want one, put a `throttle`
   node upstream of the bottleneck, or use the `null` node as a tee
   target to bleed off excess (`src -> slow, null`).

## Memory bounds

Hard upper bound on resident packet memory:

```
edges × EDGE_CAP × max_packet_size
```

For the default 64-cap and 200 edges with 1 KB packets that's ~12 MB.

## Exposing the metrics endpoint

`--metrics-port N` binds the Prometheus scrape endpoint to
`127.0.0.1:N`. This is deliberate: the endpoint has no TLS, no auth,
no rate limiting, and exposes per-node/per-edge counters that leak
flow topology. Never bind it on `0.0.0.0` and never expose it raw to
the public internet. To reach it from a remote scraper, pick one:

### SSH tunnel (ad-hoc)

```sh
ssh -L 9090:127.0.0.1:9090 edge-host
curl http://127.0.0.1:9090/metrics
```

Zero config on the jerboa host. Fine for debugging, not for a
long-running Prometheus job.

### nginx reverse proxy (recommended)

```nginx
server {
    listen 443 ssl;
    server_name metrics.example.com;

    ssl_certificate     /etc/letsencrypt/live/metrics.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/metrics.example.com/privkey.pem;

    location /jerboa/metrics {
        auth_basic           "metrics";
        auth_basic_user_file /etc/nginx/metrics.htpasswd;

        proxy_pass         http://127.0.0.1:9090/metrics;
        proxy_http_version 1.0;            # jerboa speaks HTTP/1.0
        proxy_set_header   Connection "";  # no keep-alive upstream
        proxy_read_timeout 5s;
    }
}s
```

Then in `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: jerboa
    metrics_path: /jerboa/metrics
    scheme: https
    basic_auth:
      username: prom
      password_file: /etc/prometheus/jerboa.pw
    static_configs:
      - targets: ['metrics.example.com:443']
```

### Caddy (one-liner with auto-TLS)

```
metrics.example.com {
    basicauth /jerboa/metrics {
        prom JDJhJDE0...   # `caddy hash-password`
    }
    reverse_proxy /jerboa/metrics 127.0.0.1:9090 {
        rewrite /metrics
    }
}
```

### What the proxy must add

- **TLS** — jerboa speaks plain HTTP only.
- **Auth** — basic auth, mTLS, or an allowlist. The endpoint accepts
  any GET and replies unconditionally.
- **A scrape-rate cap** — `limit_req` in nginx, or rely on
  Prometheus' own `scrape_interval`. The endpoint is cheap but not
  free; each scrape walks every node and edge under a lock.
- **A path rewrite if you proxy multiple jerboa instances** on the
  same host (`/jerboa-a/metrics`, `/jerboa-b/metrics`, ...).

### What the proxy should *not* do

- Buffer responses indefinitely — set a 5–10 s read timeout. A wedged
  jerboa shouldn't pin proxy workers.
- Keep upstream connections alive — jerboa closes after each response
  (`Connection: close`).

## Common pitfalls

- **Forgot to wire an input port.** `flow_load` fails with
  `node 'X' input port 2 has no incoming edge`. Add the edge.
- **Filter drops everything.** Common when `uppercase` upstream changes
  case; remember `filter` matches the literal substring. Lowercase
  needles match the raw packet bytes pre-uppercase.
- **Metrics unreachable from another host.** The endpoint binds to
  `127.0.0.1` only. Tunnel or reverse-proxy.
- **Lua script can't open files.** Intentional — the Lua node is
  sandboxed. Pre-compute paths in the jerboa config, or write a new
  C node.
- **MQTT messages "disappearing" under load.** The sub node has a
  fixed-capacity (1024) internal inbox; once full, new messages are
  silently dropped. Check `jerboa_queue_*` for the downstream queue
  and instrument the broker side too.

## Embedded deployment notes

Verified clean under valgrind, ASan+UBSan, and TSan. The binary uses
full RELRO, ASLR, stack canaries, FORTIFY_SOURCE=2, and stack-clash
protection. No dynamic plugin loading; no shell-out from any built-in
node (`os.execute` is sandboxed away even in Lua mode). Configuration
is file-only; there is no admin API or write path.

What is **not** in the box:
- TLS / auth (use a reverse proxy for metrics; mTLS upstream of any
  exposed MQTT broker).
- Watchdog. A pathologically slow `process()` will pin one worker.
- Log levels / log rate limiting. `fprintf(stderr, ...)` is bare.
- Hot config reload. Restart the process.
