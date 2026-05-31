# Build

## Default build

```sh
make
./jerboa flow.conf
```

Produces a single binary, `jerboa`. Default `CFLAGS`:

```
-Wall -Wextra -Werror -O2 -g -std=c11
-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fstack-clash-protection -fPIE
-Wshadow -Wformat-security -Wnull-dereference -Wdouble-promotion
```

Default `LDFLAGS`: `-pthread -pie -Wl,-z,now -Wl,-z,relro` (full RELRO + ASLR).

`-Werror` is on. Any warning fails the build.

## Optional features

| Knob | Adds | Requires |
|------|------|----------|
| `WITH_LUA=1`  | `lua` node — sandboxed Lua 5.4 per packet | `pkg-config lua5.4` |
| `WITH_MQTT=1` | `mqtt sub` / `mqtt pub` nodes             | `pkg-config libmosquitto` |
| `WITH_GPIO=1` | `gpio_in` / `gpio_out` nodes              | `pkg-config libgpiod` |

```sh
make WITH_LUA=1 WITH_MQTT=1 WITH_GPIO=1
```

## Test targets

| Target           | What it does                                       |
|------------------|----------------------------------------------------|
| `make test`           | Run the unit-test suite (26 tests)            |
| `make test-valgrind`  | Same suite under valgrind, zero-leak gate     |
| `make test-asan`      | Same suite under AddressSanitizer + UBSan     |
| `make test-tsan`      | ThreadSanitizer run of the main binary        |

All four are clean as of the last release.

## Clean

```sh
make clean
```

## Cross-compiling

On non-x86 / non-ARMv8 targets you may need `-latomic` in `LDFLAGS` for
C11 `<stdatomic.h>` operations on 64-bit integers. Native Linux
x86_64 / aarch64 needs nothing extra.
