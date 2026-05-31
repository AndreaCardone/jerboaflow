---
name: Bug report
about: A reproducible defect in jerboa
labels: bug
---

**Version / environment**
- jerboa commit: `git rev-parse HEAD` →
- OS / distro / kernel: `uname -a` →
- Compiler: `cc --version | head -1` →
- Build flags used: e.g. `make WITH_LUA=1 WITH_MQTT=1`

**Minimal `flow.conf`**

```
# paste the smallest config that reproduces
```

**Command line**

```sh
./jerboa flow.conf
```

**Expected behaviour**

What you thought would happen.

**Actual behaviour**

What happened instead. Include stderr verbatim.

**Extra diagnostics (optional but appreciated)**

- `curl http://127.0.0.1:9090/metrics` snapshot
- `valgrind --leak-check=full ./jerboa flow.conf` output
- `make test-asan` if it triggers there too
