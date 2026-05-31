---
name: Node / feature request
about: Propose a new built-in node or runtime feature
labels: enhancement
---

**What is the use case?**

Describe the real-world flow this would enable. One concrete scenario,
not a feature checklist.

**Proposed data structures**

Per [AGENTS.md](../../AGENTS.md): sketch the `struct`s first. What
state does the node hold? Who owns what? What threads touch it?

**Proposed config syntax**

```
mynode <node_type> <args...>
```

**Alternatives considered**

- Can this be done with `lua` + an existing node?
- Can it be done with `exec` piping to a script?
- If yes, why does it deserve a built-in?

**New dependencies**

- None, or:
- Name + license + Debian package + roughly how many KB it adds.
  Must live behind a `WITH_<NAME>=1` flag.
