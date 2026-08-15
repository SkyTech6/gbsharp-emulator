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
| [Ext RAM size and battery queries](#ext-ram-size-and-battery-queries) | `src/emulator.h`, `src/emulator.c` | Additive | Yes |
| [CPU location accessors](#cpu-location-accessors) | `src/emulator.h`, `src/emulator.c` | Additive | Yes |
| [Cartridge RAM bank query](#cartridge-ram-bank-query) | `src/emulator.h`, `src/emulator.c` | Additive | Yes |
| [Breakpoint bank selection](#breakpoint-bank-selection) | `src/emulator-debug.h`, `src/emulator-debug.c` | Additive | Yes |
| [Reused breakpoints keep a stale hit flag](#reused-breakpoints-keep-a-stale-hit-flag) | `src/emulator-debug.c` | Bug fix | Yes |
| [Cycle attribution in the profiler](#cycle-attribution-in-the-profiler) | `src/emulator-debug.h`, `src/emulator-debug.c` | Additive | Yes |
| [The GB# facade](#the-gb-facade) | `src/gbsharp.h`, `src/gbsharp.c` | New files | No |
| [Library targets and release pipeline](#library-targets-and-release-pipeline) | `cmake/gbsharp.cmake`, `CMakeLists.txt`, `scripts/`, `.github/workflows/` | Additive | No |
| [Trimmed upstream CI](#trimmed-upstream-ci) | `.github/workflows/build.yml`, `.github/workflows/build_release.yml` | Removal | No |
| [The GB# Player](#the-gb-player) | `player/`, `cmake/gbsharp.cmake` | New files | No |
| [The web runtime](#the-web-runtime) | `web/`, `cmake/gbsharp.cmake` | New files | No |
| [Forced checkout of test repositories](#forced-checkout-of-test-repositories) | `scripts/build_tests.py` | One word | Yes |

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

## Ext RAM size and battery queries

**Files:** `src/emulator.h`, `src/emulator.c` (two new functions)

The facade has to answer "how many bytes of save RAM does this cartridge
have, and is any of it worth writing to disk" without exposing `Emulator`.
Upstream's only route to the size is `emulator_init_ext_ram_file_data`, which
allocates a buffer as a side effect of being asked a question, and there is no
route at all to the battery flag even though `emulator_read_ext_ram` and
`emulator_write_ext_ram` already branch on it.

`emulator_get_ext_ram_size` and `emulator_has_battery` are two accessors over
state that is already there.

**Upstream candidate:** yes.

## CPU location accessors

**Files:** `src/emulator.h`, `src/emulator.c`, `src/emulator-debug.h`,
`src/emulator-debug.c`

GB# turns a running address back into the C# line that produced it, by way of
the bank and address pair a linker's `.sym` file records. Both halves of that
pair already existed; neither could be asked for.

`emulator_get_PC` is defined in `emulator.c` and was declared by no header,
exactly the situation `emulator_read_mem` was in before the patch above. It is
declared, not moved.

`emulator_get_rom_bank` moves from `emulator-debug.c` to `emulator.c`, and its
declaration from `emulator-debug.h` to `emulator.h`, with the body unchanged.
It reads `MemoryMapState::rom_base`, which every build maintains and every MBC
bank select updates, so nothing about it needed the instrumented flavour in
the first place. The debugger still sees it, through the `emulator.h` that
`emulator-debug.h` already includes.

That is the whole point of the patch: asking where the CPU is should not
require the instrumented build, because a caller that had to pay for hooks to
read a register would simply never read it.

**Upstream candidate:** yes. One declaration of an existing function, and one
existing function compiled into both flavours instead of one.

## Cartridge RAM bank query

**Files:** `src/emulator.h`, `src/emulator.c` (one new function)

`emulator_get_ext_ram_bank`, the counterpart of `emulator_get_rom_bank` above,
reading `MemoryMapState::ext_ram_base` the same way and for the same reason.
With the ROM bank it is all of the MBC state a caller can act on: everything
else an MBC holds is latched input to these two.

**Upstream candidate:** yes. An accessor over existing state.

## Breakpoint bank selection

**Files:** `src/emulator-debug.h`, `src/emulator-debug.c` (one new function)

`emulator_set_breakpoint_address` records `emulator_get_rom_bank(e, addr)` —
the bank that happens to be mapped as the call is made. There is therefore no
way to set a breakpoint on code in a bank that is not currently switched in,
which is the normal case for GB#, because a breakpoint placed from a linker
map is placed before the code has ever run.

`emulator_set_breakpoint_bank` sets the field directly. No mask to recalculate:
the breakpoint mask is over addresses, and the bank is compared separately once
an address matches.

**Upstream candidate:** yes. It closes a hole in upstream's own breakpoint API,
which its debugger works around by only breaking in mapped banks.

## Reused breakpoints keep a stale hit flag

**Files:** `src/emulator-debug.c` (one line)

`hit_breakpoint` skips a breakpoint whose `hit` flag is already set, so that
stopping at one and resuming does not immediately stop at it again. The flag is
cleared by that skip, not by removal, and `emulator_add_empty_breakpoint`
resets every field of a recycled slot except this one.

A breakpoint that lands in a slot whose previous occupant was hit therefore
misses the first time it is reached. For an address reached repeatedly the
symptom is one skipped stop and nothing worse, which is presumably why it has
gone unnoticed. For an address reached exactly once — the entry point of a
function that runs once and then loops, which is precisely what GB# places
breakpoints on from a linker map — the breakpoint never fires at all.

Found by a test that passed alone and failed in a suite, which is the shape
this bug has to have: it needs a previous breakpoint to have been hit.

**Upstream candidate:** yes. A one-line fix to a real bug.

## Cycle attribution in the profiler

**Files:** `src/emulator-debug.h`, `src/emulator-debug.c`

Upstream counts how many times the instruction at each ROM address ran, in
`s_profiling_counters`. A count is not a cost, and the two rank code
differently: a handful of 20 tick calls outweighs a great many 4 tick loads,
so a profile by count points at the wrong code. Anything asking "where did the
frame budget go" wants ticks.

`s_profiling_cycles` is a second array of the same shape, filled in the hook
that already fires per instruction. An instruction's cost is measured from its
own hook to the next one, because that is the only point at which the clock has
finished advancing for it. Interrupt dispatch therefore lands on the
instruction it interrupted, which is where it belongs: that instruction is when
the cost was paid, whoever asked for it. Code executing from RAM has no ROM
address to bill, so it closes the previous measurement and opens none.

Also adds `emulator_clear_profiling_counters`, which upstream never needed
because its debugger reads the counters and never resets them. A profile of one
scene rather than of a whole session needs a reset, and clearing has to also
drop the in-flight measurement or the first instruction after it is billed for
every tick since the last one.

This is the one patch here that adds state rather than exposing it: another
`MAXIMUM_ROM_SIZE` array of `u32`, so the instrumented flavour's BSS grows by
32MB. It is zero fill, so the pages are committed only for the part of the ROM
that actually executes, and only in the flavour that is already paying for
hooks on every instruction.

**Upstream candidate:** yes, though it is the largest of these and the one
upstream is most likely to want to shape differently.

## The GB# facade

**Files:** `src/gbsharp.h`, `src/gbsharp.c` (both new)

The ABI GB# talks to, and the reason this fork exists. `gbsharp_emulator` is
opaque and nothing from `emulator.h` appears in the header, so the emulator
underneath can be replaced without the C# side noticing.

Three things in it are deliberate rather than convenient:

- **A ROM arrives as bytes, never as a path.** The core does not open files, so
  storage policy belongs to the host. That is what lets one core serve a native
  player, a browser and a test that touches no disk.
- **`gbsharp_run_frame` advances 70224 ticks against an absolute deadline** and
  consults no clock, then runs on to the end of the frame the PPU is drawing so
  that the framebuffer never holds half of two frames. Upstream's `tester.c`
  waits for that frame with no bound and would spin forever with the display
  off; the facade allows one extra frame's ticks and then gives up. With that
  loop the facade reproduces every screen hash in `scripts/test.json` for the
  blargg suite byte for byte, which is the evidence that the ABI did not change
  emulation.
- **Audio is pulled.** `emulator_run_until` already stops when its buffer
  fills; the facade drains that buffer after each frame and discards whatever
  the host did not take. Upstream's `host.c` drives audio from an SDL callback,
  which would tie emulation progress to a device and take determinism with it.

`gbsharp_reset` builds a new `Emulator` over a fresh copy of the cartridge,
because upstream has no reset and a power cycle is what reset means anyway. The
cartridge copy is kept on the facade side for exactly this reason:
`emulator_new` takes ownership of the buffer it is handed and may reallocate
it, so the same pointer cannot be handed over twice.

Nothing in either file changes upstream behaviour, and no upstream file
includes them.

**Upstream candidate:** no. It is a GB# ABI, not an emulator feature.

## Library targets and release pipeline

**Files:** `cmake/gbsharp.cmake` (new), `CMakeLists.txt` (one `include`),
`scripts/check_gbsharp_library.py` (new),
`.github/workflows/gbsharp.yml` (new),
`.github/workflows/gbsharp-release.yml` (new)

Builds the facade as a shared and a static library, in both flavours, with no
SDL, no OpenGL and no imgui in the link:

    gbsharp_emulator         emulator.c, no instrumentation
    gbsharp_emulator_debug   emulator-debug.c, which includes emulator.c and
                             compiles it again with hooks enabled

Instrumentation is a compile time choice upstream, so it has to be two
libraries rather than one flag. Both export the identical ABI and
`gbsharp_has_debug_support` says which one loaded, which is what lets a single
set of P/Invoke declarations serve both and lets GB# ship the fast one to
players and the hooked one to tooling.

The target definitions live in their own file, included from one line at the
bottom of `CMakeLists.txt`, so upstream can rewrite that file freely and the
only thing to reapply is that line.

Two things enforce the no-SDL boundary rather than documenting it. The shared
libraries are linked with `--no-undefined` on ELF, where undefined symbols
would otherwise be deferred to load time, so adding `host.c` to the core
sources becomes a link error on every platform. `scripts/check_gbsharp_library.py`
then scans the built library for SDL, OpenGL and imgui, and for the full list
of exported facade symbols, which catches the case where somebody adds the
link libraries too. Hidden visibility keeps the export list to the facade
itself, so the ABI is the header rather than whatever `emulator.c` left
non-static.

The release workflow builds `win-x64`, `linux-x64`, `linux-arm64`, `osx-x64`
and `osx-arm64` on a tag and publishes archives plus an `emulator.lock.json`
generated from the archives it actually uploaded. GB# verifies that checksum
before it will load a library, and almost no GB# contributor will ever compile
this repository, so CI is the only thing between a broken commit and a broken
release.

**Upstream candidate:** no.

## Trimmed upstream CI

**Files:** `.github/workflows/build.yml`, `.github/workflows/build_release.yml`
(deleted)

Upstream's CI serves upstream's audience, which includes projects GB# is not.
What was removed and why:

- **`build_release.yml`, deleted.** It builds the SDL application and uploads
  tarballs to any release that gets created. GB# releases the runtime through
  `gbsharp-release.yml`, so leaving this in place meant a GB# runtime release
  could acquire `binjgb-ubuntu.tar.gz` as an asset. It has not happened only
  because releases created with `GITHUB_TOKEN` do not trigger further
  workflows; a release created by hand would have.
- **The `rgbds-live` and `gbstudio` wasm variants, removed.** Both exist for
  named downstream projects with their own compile-time configuration. GB# is
  neither, and its web runtime will be its own target through the facade. The
  plain wasm build stays, as the check that emcc can compile this core.
- **The tag trigger on `build.yml`, removed.** `on: create: tags:` fired on
  `gbsharp-v*` release tags and rebuilt the application, the imgui debugger and
  the wasm module, none of which a runtime release contains.
- **The compatibility suite, removed from `build.yml`.** It now runs once, in
  `gbsharp.yml`, which is where a GB# patch changing emulation has to fail.
  Running it twice more on two platforms was not adding a signal.

What stays is the build of upstream's own application and wasm target on every
commit. That is worth keeping: it is what makes a rebase onto `upstream-main`
trustworthy, and milestone 2's Player reuses the video and audio paths from
`host.c`, so those files need to keep compiling.

**Upstream candidate:** no. Upstream needs all of it.

## The GB# Player

**Files:** `player/main.c`, `player/payload.c`, `player/payload.h`,
`player/config.c`, `player/config.h` (all new), `cmake/gbsharp.cmake` (one
target)

What a published GB# game is: a window, a sound device, a joypad, and nothing
else. No ROM browser, no save states, no settings, no menu, because a player
that offered those would be an emulator that happens to have a game in it.

Three decisions in it are worth recording.

**It goes through `gbsharp.h` rather than through `emulator.h`.** It sits in
the same repository as the core and could link it directly. Reaching the
emulator only through the ABI means the boundary is exercised by the thing
users actually run, so a change that breaks the ABI breaks the Player too
instead of quietly working here and failing in the browser and the test
harness.

**A game is one executable with its ROM appended to it.** `gbsharp publish`
copies this stub and appends the ROM, the window settings, and a trailer; the
Player reads its own file at startup and loads the ROM from memory through the
same `gbsharp_load_rom` a test uses. Nothing is unpacked to disk and nothing is
relinked, which is what lets publishing work for users with no C toolchain and
work identically on every platform. `player/payload.h` documents the format.
The costs, accepted knowingly: code signing has to happen after the append, so
it belongs to `gbsharp publish`; and the icon and version metadata are baked
into this stub.

**The display does not pace the emulator.** A frame is due every 16.742706ms
because that is 70224 ticks at 4194304Hz, and vsync only stops tearing. Pacing
on the display instead would run a game 2.4 times too fast on a 144Hz monitor.

`--frames` and `--screenshot` exist for CI, which needs to know that a stub
starts and draws rather than merely failing to crash. Under `--frames` the
error paths report through an exit code instead of a message box, since
nothing is there to dismiss one.

The target is built only when SDL2 is present, so the runtime, which is what
almost everybody consumes, still builds on a machine with no SDL at all.

**Upstream candidate:** no.

## The web runtime

**Files:** `web/gbsharp-runtime.js`, `web/check-wasm.mjs` (both new),
`cmake/gbsharp.cmake` (an emscripten branch)

The same `src/gbsharp.c` and the same `src/emulator.c` through emcc, plus a JS
module in which every method is one entry point of `gbsharp.h` under the same
name. That correspondence is the point: a host written against the C ABI and a
host written against this one are the same program, so somebody reading either
can follow the other.

Upstream already has `src/emscripten/wrapper.c`, and this does not use it. That
wrapper exposes `Emulator*` and upstream's own function set, which is precisely
what the facade exists to hide; going through it would have given the browser a
different emulator interface from the one the native player and the test
harness use, and then there would be two.

`malloc` and `free` are exported alongside the facade because a ROM has to be
copied into the module's heap before `gbsharp_load_rom` can be handed a pointer
to it. That is the browser's version of "a ROM arrives as bytes, never as a
path", and `-sFILESYSTEM=0` makes sure no filesystem is linked in to pretend
otherwise.

`web/check-wasm.mjs` runs a ROM through the module under node and prints the
SHA1 of the screen in the format `tester.c` writes, so CI compares the wasm
build against the hashes upstream records for its own tester rather than
against itself. All four blargg ROMs it runs match, as does the native build,
which is the evidence that there is one emulator here and not two.

**Upstream candidate:** no.

## Forced checkout of test repositories

**Files:** `scripts/build_tests.py` (one argument)

`GitUpdate` clones wla-dx and the mooneye test ROMs and checks out a pinned
sha. A plain `git checkout` refuses when the working tree differs from HEAD,
and a fresh clone manages that on its own through line ending normalisation, so
CI started failing to build the test ROMs for a reason with nothing to do with
the emulator.

These are scratch clones of somebody else's repository and the pinned sha is
the only thing about them anybody cares about, so `--force` is what the code
meant in the first place.

**Upstream candidate:** yes.
