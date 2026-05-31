# Changelog

All notable changes to jerboa are recorded here. Format roughly follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning
follows [SemVer](https://semver.org/) once we reach 1.0.

## [Unreleased]

### Added
- `logic` worker node: compares the whole payload against a literal
  (`== != < <= > >= contains`) and routes by emitting `=` (forward),
  `!` (drop), or any literal token on each branch. Quoted-string args
  supported so compare values and emitted literals may contain spaces.
- `random` worker node: replaces each input with a random decimal
  integer in the inclusive range `[min, max]`. Per-node `rand_r` seed,
  no global state.
- `delay` worker node: forwards each packet after sleeping
  `<interval_ms>` ms. Order-preserving on a single input; unlike
  `throttle`, no packets are dropped.
- `gpio_in` source node: edge events (`rising`/`falling`/`both`) on a
  Linux GPIO line via libgpiod, emitting `"1"` or `"0"` per event.
  200 ms `gpiod_line_event_wait` tick keeps shutdown responsive.
  Opt-in via `WITH_GPIO=1`.
- `epoll_in` source node: watches a path (FIFO, char device, socket --
  anything epoll(7) accepts) and emits one packet per newline-terminated
  line. CRLF tolerant. FIFOs are opened `O_RDWR` so external writer
  churn does not terminate the listener. Oversized lines (>4 KiB) are
  flushed with a warning -- bytes are never silently dropped. Regular
  files are not supported (epoll rejects them).
- Examples: `19_logic.conf` (route/drop/relabel with `logic`) and
  `20_sensor_merge.conf` + `20_sensor_merge.lua` (three sensor files
  joined and JSON-encoded in Lua with threshold-based alarms).
- `docs/nodes.md` rows for `logic`, `random`, `delay`, `gpio_in`,
  `gpio_out`, `epoll_in`.
- `docs/operations.md`: full "Exposing the metrics endpoint" section
  (SSH tunnel, nginx reverse proxy with TLS + basic auth, Caddy
  one-liner, proxy do's and don'ts).

### Changed
- `gpio` sink split into separate `gpio_in` (source) and `gpio_out`
  (sink) node types, both registered from `nodes/gpio.c`. The old
  `gpio` name is gone; configs using it must be updated.
- Internal: extracted shared HTTP/1.0 plumbing (write_all, socket
  timeouts, fixed-status response, header-read loop, accept/serve
  skeleton) into `http_io.{c,h}`. `nodes/http.c`, `nodes/http_out.c`,
  and `metrics.c` now share one implementation of the listener loop
  and response helpers. No behaviour change, no new dependencies.
- Internal: promoted the full HTTP client (URL parsing, dial, send,
  drain, status-line parse) from `nodes/http_out.c` into `http_io` as
  `http_url_parse` + `http_client_do` + `HttpResponse`. `http_out.c`
  is now just policy: pick fields, log non-2xx/truncated, emit a
  packet.

### Removed
- `transform` node (superseded by `lua` for arbitrary per-packet
  rewriting and by `logic` for compare-and-route).

## [0.1.0-beta] - 2026-05-30

First public preview release.

### Added
- Core runtime: bounded packet queues, fixed worker pool, DAG scheduling,
  per-edge backpressure, graceful shutdown via SIGINT/SIGTERM.
- Configuration loader with single-file flat syntax.
- Built-in nodes: `generator`, `file_source`, `http_in`, `mqtt sub`,
  `uppercase`, `filter`, `filter_re`, `batch`, `split`, `throttle`,
  `count`, `tee`, `exec`, `lua`, `printer`, `fwriter`, `file_sink`,
  `http_out`, `mqtt pub`, `gpio`, `null`.
- Optional features: `WITH_LUA=1` (Lua 5.4 sandbox + bundled
  rxi/json.lua exposed as global `json`), `WITH_MQTT=1` (libmosquitto),
  `WITH_GPIO=1` (libgpiod).
- Prometheus text-format metrics endpoint (`--metrics-port N`,
  loopback-only).
- 27 unit + integration tests (packet, pqueue, flow, nodes); valgrind,
  ASan/UBSan, and TSan suites all clean.
- 16 example flows covering hello-world, fan-out/join, batch/throttle,
  Lua scripting, MQTT round-trips, HTTP webhooks, GPIO, exec, and a
  201-node stress DAG.
- Hardened build: `-Wall -Wextra -Werror`, `-fstack-protector-strong`,
  `-D_FORTIFY_SOURCE=2`, `-fstack-clash-protection`, `-fPIE`, full RELRO
  and BIND_NOW at link time.
- `--version` / `-V` CLI flag.

### Known limitations
- No TLS on any transport (MQTT, HTTP in/out).
- No authentication on `http_in`, no username/password on MQTT nodes.
- No secrets handling — credentials referenced in `flow.conf` are
  plaintext on disk. Don't put any there yet.
- Queues are in-memory only; in-flight packets are lost on crash.
- No hot reload of `flow.conf` — restart the process.
- No internal watchdog or liveness endpoint: a wedged node is not detected or auto-restarted. Use a process supervisor (e.g. systemd `Restart=on-failure`).
- Linux-only (tested on x86_64 and aarch64).
- Config grammar is **not** stable until 1.0.

### Planned for 0.2
- `${VAR}` env-var and `@file` substitution in `flow.conf`.
- MQTT TLS + username/password auth.
- `http_out` TLS.
- `http_in` TLS.
