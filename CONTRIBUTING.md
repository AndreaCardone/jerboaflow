# Contributing to jerboa

Patches, bug reports, and node ideas welcome.

## Ground rules

jerboa is deliberately small and opinionated. Before opening a PR, read
[AGENTS.md](AGENTS.md) — the style charter is enforced, not suggested.

Short version:

- **C11, no compiler extensions.** Builds with `-Wall -Wextra -Werror
  -O2 -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -std=c11`. If a
  warning appears, fix the cause, do not suppress it.
- **No new runtime dependencies** without prior discussion. Optional
  features go behind a `WITH_*` flag, exactly like `WITH_LUA` and
  `WITH_MQTT`.
- **Data structures first.** For a new node or feature, sketch the
  `struct`s and ownership rules in the issue before writing the
  implementation.
- **Self-contained modules.** One node per file in `nodes/`. No
  helper-library sprawl.
- **No banned libc.** `strcpy`, `strcat`, `sprintf`, `gets` are
  rejected at review. Use length-carrying buffers and bounded variants.
- **Comments explain WHY, not WHAT.** Code should read itself.

## Before submitting a PR

```sh
make clean
make jerboa WITH_LUA=1 WITH_MQTT=1     # must compile clean
make test WITH_LUA=1 WITH_MQTT=1       # all tests pass
make test-valgrind WITH_LUA=1 WITH_MQTT=1   # zero leaks, zero errors
make test-asan WITH_LUA=1 WITH_MQTT=1  # ASan + UBSan clean
```

CI runs the same matrix. A red CI run will not be merged.

New nodes need:

1. A file in `nodes/<name>.c` registered as `const NodeType ndt_<name>`.
2. An `extern` line in `nodes/nodes.h`.
3. An entry in the `all[]` array in `nodes/builtins.c`.
4. A unit test in `test/test_nodes.c` covering the happy path.
5. A row in the table in `docs/nodes.md`.
6. A `CHANGELOG.md` entry under `[Unreleased] / Added`.

## Reporting bugs

Open an issue with:

- jerboa version (commit SHA), OS, libc, compiler version
- the minimal `flow.conf` that reproduces
- exact command line
- stderr output (and `/metrics` snapshot if relevant)
- valgrind / ASan output if you have it

## Security issues

Do not file public issues for vulnerabilities. Email the maintainer
listed in `git log -1 --format=%ae` on the latest tagged release.
