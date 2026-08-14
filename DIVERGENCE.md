# Divergence from upstream binjgb

Everything this fork changes, one section per patch. A patch that cannot be
justified in writing here is a patch to drop.

Upstream is `binji/binjgb`; the fork point is recorded in [NOTICE](NOTICE).
The authoritative list of changes is:

```
git log --oneline upstream-main..main
```

Each patch below says what it touches, why GB# needs it, and whether it is a
candidate to send upstream. "Additive" means the patch only adds files or
symbols and changes no existing behaviour, which is what keeps
`git rebase --onto upstream-main` cheap.

| Patch | Files | Kind | Upstream candidate |
|---|---|---|---|
| [Fork lineage](#fork-lineage) | `NOTICE`, `DIVERGENCE.md` | Additive | No |
| [Silenceable cart info log](#silenceable-cart-info-log) | `src/emulator.h`, `src/emulator.c` | Additive | Yes |
| [Declared memory accessors](#declared-memory-accessors) | `src/emulator.h` | Additive | Yes |

## Fork lineage

**Files:** `NOTICE`, `DIVERGENCE.md`

Records what this fork is, what it was forked from, and how its branches are
meant to be used. Nothing else in the tree is touched, and neither file exists
upstream, so this patch can never conflict.

**Upstream candidate:** no. It is about the fork, not about binjgb.

## Silenceable cart info log

**Files:** `src/emulator.h` (one field), `src/emulator.c` (one call site)

`emulator_new` writes ten lines of cart info to stdout through
`log_cart_info`. That is useful in a command line emulator and wrong in a
library: GB# creates an emulator per test, and a few hundred of those turn the
test log into cart headers. Worse, a library that writes to stdout cannot be
embedded in a tool whose stdout is its output.

Adds `EmulatorInit::quiet_cart_info` and calls `log_cart_info` only when it is
false. The flag is inverted on purpose so that zero means "log", which is what
a `ZERO_MEMORY(init)` caller such as `binjgb.c` or `tester.c` already gets.
No upstream behaviour changes.

Per instance rather than a global, so it stays correct when a process holds
several emulators on several threads, which the GB# test suite does.

**Upstream candidate:** yes. Small, additive, and useful to any embedder.

## Declared memory accessors

**Files:** `src/emulator.h` (two declarations)

`emulator_read_mem` and `emulator_write_mem` are already defined in
`emulator.c`, but no header declares them; only `src/emscripten/exported.json`
names them. The facade needs untimed memory access in both library flavours,
and `emulator_read_u8_raw` is not an option because it lives in
`emulator-debug.c` and so exists in the debug flavour only.

Declaring them rather than repeating the prototype in `gbsharp.c` means the
compiler checks the prototype against the definition. A duplicated extern
would go stale silently.

**Upstream candidate:** yes. It declares what already exists.
