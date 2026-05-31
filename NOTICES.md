# Third-party notices

jerboa is BSD-2-Clause licensed (see [LICENSE](LICENSE)). It bundles
and/or links the following third-party components:

## Bundled (in-tree)

### rxi/json.lua

A pure-Lua JSON encoder/decoder, embedded at
[nodes/embed/json.lua](nodes/embed/json.lua) and compiled into the
binary as a string literal in [nodes/embed/json_lua.h](nodes/embed/json_lua.h).
Loaded once per `lua` node and exposed to user scripts as the global
`json` table (`json.decode`, `json.encode`).

- Upstream: <https://github.com/rxi/json.lua>
- License: MIT
- Copyright (c) 2020 rxi

The full license text is preserved at the top of `nodes/embed/json.lua`.

## Linked at runtime (optional)

These libraries are dynamically linked only when the matching build flag
is enabled. They are **not** bundled — you must install them
separately.

### Lua 5.4 — `WITH_LUA=1`

- Upstream: <https://www.lua.org/>
- License: MIT
- Copyright © 1994–2024 Lua.org, PUC-Rio.

### libmosquitto — `WITH_MQTT=1`

- Upstream: <https://mosquitto.org/>
- License: EPL-2.0 / EDL-1.0 (dual)
- Copyright © the Mosquitto authors.

### libgpiod — `WITH_GPIO=1`

- Upstream: <https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/>
- License: LGPL-2.1-or-later
- Copyright © the libgpiod authors.

---

If you spot a missing attribution, please open an issue.
