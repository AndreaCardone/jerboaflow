# Built-in nodes

Always-on (built into the default binary):

| type         | role     | args                                     | summary |
|--------------|----------|------------------------------------------|---------|
| `generator`  | source   | `<interval_ms> <count> <text>`           | Emit `<text>-N` every interval. `count=0` is infinite. |
| `file_source`| source   | `<path>`                                 | Emit each line of a file as a packet, then terminate. |
| `uppercase`  | worker   | —                                        | Uppercase the payload in place (copy-on-write if shared). |
| `filter`     | worker   | `<needle>`                               | Forward packets whose payload contains `needle` (substring; max 63 bytes). |
| `filter_re`  | worker   | `<regex>`                                | Forward packets whose payload matches a POSIX extended regex (`REG_EXTENDED \| REG_NOSUB`). Embedded NULs are remapped to `\x01` before matching. |
| `logic`      | worker   | `<op> <compare> <true_action> <false_action>` | Compare the whole payload, then `=` forward, `!` drop, or emit a literal token. Quote any field to include spaces. Ops: `== != < <= > >= contains`. |
| `batch`      | worker   | `<N>` (default 10)                       | Coalesce N consecutive packets, comma-joined. Partial batch at shutdown is dropped. |
| `split`      | worker   | `<delim>` (default `\n`; first char only)| Split payload on delim; emit one packet per non-empty piece. |
| `throttle`   | worker   | `<interval_ms>` (default 1000)           | Forward at most one packet per interval; drop the rest. |
| `count`      | worker   | —                                        | Emit a decimal text packet with the running count (1, 2, 3, ...). |
| `random`     | worker   | `<min> <max>`                            | Replace each input with a random decimal integer in the inclusive range `[min, max]`. |
| `tee`        | worker   | —                                        | Pure pass-through (useful with fan-out for debugging). |
| `printer`    | sink     | `<prefix>` (default `out`)               | Print `[name/prefix in=K] <payload>` to stdout. |
| `file_sink`  | sink     | `<path>`                                 | Append payload + `\n` to a file. |
| `http_in`    | source   | `<bind_ipv4> <port> [path]`              | HTTP/1.0 listener; each request body becomes a packet, reply is always `204 No Content`. |
| `epoll_in`   | source   | `<path>`                                 | epoll(7) on `<path>` (FIFO, char device, socket); emits one packet per `\n`-terminated line (CRLF tolerant). FIFOs opened `O_RDWR` so writer churn does not terminate the listener. Regular files are not supported (epoll rejects them). |
| `http_out`   | worker   | `<url> [method] [content-type]`          | One HTTP/1.0 request per input packet (default `POST`); response body re-emitted on success. |
| `exec`       | worker   | `[-t <ms>] <shell-command...>`           | Per packet: fork `/bin/sh -c <cmd>`, pipe payload to stdin, emit captured stdout. Optional wall-clock timeout. |
| `null`       | sink     | —                                        | Drop every packet. The `/dev/null` of jerboa. |

## Optional (feature-gated)

### `lua` — requires `WITH_LUA=1`

```
mynode lua <script_path> [mem_kb]
```

- `script_path`: Lua 5.4 file with a global `function process(payload, in_idx)`
  that returns a string (emit) or `nil` (drop).
- `mem_kb`: hard memory cap per node, default 4096 (4 MB). Set `0` for unlimited.
- **Sandboxed**: only `base`, `string`, `math`, `table`, `utf8` libraries are
  exposed. `io`, `os`, `package`, `debug` are absent; `dofile`, `loadfile`,
  `load`, `loadstring`, `require`, `collectgarbage` are removed from globals.

### `mqtt` — requires `WITH_MQTT=1`

```
sub  mqtt sub <host> <port> <topic>     # source: emit each received message
pub  mqtt pub <host> <port> <topic>     # sink:   publish each input
```

- One MQTT client per node. Sub mode runs `mosquitto_loop_start` in its own
  thread and pushes received messages onto an internal bounded inbox
  (capacity 1024 packets); a slow downstream causes new messages to be
  dropped silently — instrument via the queue metrics.
- libmosquitto's global init/cleanup is refcounted across nodes.

### `gpio_in` / `gpio_out` — requires `WITH_GPIO=1`

```
btn  gpio_in  <chip> <line> rising|falling|both    # source: emit "1" or "0"
led  gpio_out <chip> <line> [initial]              # sink:   set line per payload
```

- `<chip>` accepts `gpiochip0`, `0`, or `/dev/gpiochip0`.
- `gpio_in` uses libgpiod edge events; on each event the payload is the
  single character `"1"` (rising) or `"0"` (falling). The internal
  `gpiod_line_event_wait` uses a 200ms tick so shutdown stays responsive.
- `gpio_out` sets the line high if the first non-whitespace byte of the
  incoming payload is `'1'`, `'h'`, `'H'`, `'t'`, or `'T'`; otherwise low.
  `[initial]` (`0` or `1`, default `0`) is the value driven at startup.
- jerboa holds the line until the flow is freed. Run as a user that can
  open `/dev/gpiochipN` (group `gpio` on most distros, or with
  `CAP_SYS_RAWIO`).

## Source vs. worker

The runtime treats a node as a **source** iff `n_in == 0` and `n_out > 0`.
Sources get a dedicated thread; workers run on the shared pool. A node
type's `init` decides this implicitly via the edges declared in the config.

## Notes on `http_in`

```
hook http_in 127.0.0.1 8080 /ingest
```

- `bind_ipv4` must be a dotted IPv4 address (no DNS, no hostnames). Use
  `127.0.0.1` for loopback only, or `0.0.0.0` to accept on every
  interface. There is no default — the address is mandatory because the
  security implications differ.
- `path` defaults to `/`. Requests on any other path get `404`. Query
  strings are stripped for matching: `/ingest?x=1` still matches
  `/ingest`.
- The request body becomes the packet payload. `Content-Length` is
  required for non-empty bodies; chunked transfer encoding is not
  supported. Bodies above 1 MiB return `413`.
- The internal inbox is bounded (1024 packets). When full, the listener
  thread blocks on push, applying TCP-level backpressure to clients.
- No TLS, no auth, no keep-alive. Put a reverse proxy in front
  ([nginx](https://nginx.org), [caddy](https://caddyserver.com)) if you
  need any of those.
- The response is **always** `204 No Content` on success — jerboa cannot
  forward downstream results back to the HTTP client.

## Notes on `http_out`

```
post http_out http://api.example.com/ingest
put  http_out http://10.0.0.5:8080/state PUT application/json
```

- `url` must be `http://host[:port]/path`. **Plain HTTP only** — there
  is no TLS, no redirect following, no auth header support. If you need
  any of those, point at a local proxy (nginx, haproxy, `caddy reverse-proxy`)
  that adds them.
- `method` defaults to `POST`. Any all-ASCII-letters token is accepted
  and uppercased (`get`, `Put`, `PATCH` all work).
- `content-type` defaults to `application/octet-stream`. If your value
  has spaces (e.g. `text/plain; charset=utf-8`), it's still parsed
  correctly — everything after the method token is treated as the
  content type.
- One TCP connection per packet, `Connection: close`. Connect/recv/send
  timeout is 5 seconds.
- The response body (capped at 1 MiB) is emitted as a new packet **if**
  the node has any outgoing edges. Non-2xx responses log to stderr but
  the body is still forwarded — 4xx/5xx bodies are usually the
  diagnostic. Network failures log to stderr and emit nothing.
- One blocking I/O call per packet; the runtime parallelises across
  worker threads. For very high-rate fan-out to the same endpoint,
  consider batching upstream with `batch`.

## Notes on `exec`

```
e exec jq -r .level
e exec -t 2000 jq -r .level     # kill child after 2 s wall time
```

- The string after `exec` (after the optional `-t <ms>`) is passed to
  `/bin/sh -c` verbatim, so the shell interprets pipes, redirections,
  quoting, and `$VAR` expansion. Treat the config file as code:
  anything that can edit it can run arbitrary commands as the jerboa
  user.
- One fork + exec **per input packet**. Fine for hundreds of
  packets/sec, ruinous for hundreds of thousands. If you need
  per-message transformation at line rate, use `uppercase`, `lua`, or
  write a node in C.
- stdin write and stdout read are interleaved via `poll()`, so a child
  that fills its stdout pipe before draining its stdin will not
  deadlock.
- `-t <ms>` (1..3600000) bounds the **entire** child lifetime
  (stdin write + stdout drain + exit). On expiry the child gets
  `SIGTERM`, then `SIGKILL` after a 200ms grace; any output already
  collected is still emitted, and an error is logged.
- The captured stdout is emitted as a single packet, capped at 1 MiB.
  Output beyond the cap is silently discarded so the child does not
  block on `write()`.
- stderr is left attached to jerboa's own stderr — errors from the
  child show up in your logs unchanged.
- A non-zero exit status is logged to stderr but the output is
  still emitted (some tools write useful diagnostics on failure).
  Use `filter` downstream if you need to drop those.
