# gbsharp-emulator

The Game Boy runtime behind [GB#](https://github.com/SkyTech6/GBSharp), a C# to
Game Boy compiler.

This repository is a fork of **[binjgb](https://github.com/binji/binjgb)** by
Ben Smith, MIT licensed. The emulation is his work; see [NOTICE](NOTICE) for the
attribution that any distribution embedding this runtime must carry, including
games published by GB#.

## What this fork is for

GB# needs an emulator it can embed rather than an emulator it can run: one that
loads a ROM from memory, advances exactly one frame when asked, and reports
where the CPU is so a running address can be turned back into the C# line that
produced it. binjgb already emulates; what it did not have was a boundary a host
could link against. That boundary is what this fork adds.

* **A stable C ABI**, `src/gbsharp.h`. `gbsharp_emulator` is opaque and nothing
  from `emulator.h` appears in the header, so the same P/Invoke declarations
  serve every consumer and the core underneath can change without the C# side
  noticing. A ROM arrives as bytes, never as a path — the core opens no files,
  so storage policy stays with the host. Audio is pulled rather than pushed from
  a device callback, which is what keeps a run deterministic.
* **Libraries with no SDL, OpenGL or imgui in the link**, in two flavours:
  `gbsharp_emulator` (plain) and `gbsharp_emulator_debug` (instrumented, with
  breakpoints and a cycle-attributing profiler). Instrumentation is a
  compile-time choice upstream, so it has to be two libraries;
  `gbsharp_has_debug_support` says which one loaded. GB# ships the fast one to
  players and the hooked one to tooling.
* **The GB# Player** (`player/`), what a published GB# game actually is: a
  window, a sound device, a joypad, and nothing else. `gbsharp publish` copies
  this stub and appends the ROM and window settings to it, so publishing needs
  no C toolchain on the author's machine. The payload format is documented in
  `player/payload.h`.
* **The web runtime** (`web/gbsharp-runtime.js`), the same sources through
  emscripten, with one JS method per entry point of `gbsharp.h` under the same
  name — so a host written against the C ABI and one written against the browser
  module are the same program.

A handful of small accessors were added to the core itself (ROM and ext-RAM bank,
PC, ext-RAM size, battery presence, breakpoint bank selection, per-instruction
cycle counts). They expose state binjgb already maintained. Everything else in
`src/` is upstream's, with its copyright headers untouched; files added here
carry their own.

Upstream's own application, debugger and wasm build still build and are still
covered by CI. If you want a Game Boy emulator to *use*, use
[binjgb](https://github.com/binji/binjgb) — it is the better front end and it is
where fixes to the emulation should go.

## Consuming it

Almost nobody needs to build this. GB# downloads a tagged release
(`gbsharp-v*`), verifies it against the `emulator.lock.json` published with it,
and loads the library. Build from source only when changing the runtime itself.

## Building

Requires [CMake](https://cmake.org). SDL2 is needed only for the Player and for
upstream's own application; the libraries build without it.

```
$ git clone --recursive https://github.com/SkyTech6/gbsharp-emulator
$ mkdir build && cd build
$ cmake .. && cmake --build .
```

On Windows, point CMake at SDL2 if you want the Player:

```
> cmake .. -DSDL2_ROOT_DIR="C:\path\to\SDL\"
```

For the web runtime, build with the emscripten toolchain; `web/check-wasm.mjs`
runs a ROM through the resulting module under node and prints the screen hash in
the format upstream's `tester.c` writes.

`scripts/check_gbsharp_library.py` scans a built library for SDL, OpenGL and
imgui symbols and for the full facade export list. It is what enforces the
no-SDL boundary rather than merely documenting it.

## Tests

`scripts/build_tests.py` downloads and builds the test suites;
`scripts/tester.py` runs them, filtered by a command line argument:

```
$ scripts/tester.py          # everything
$ scripts/tester.py mooneye
```

The facade reproduces every screen hash in `scripts/test.json` for the blargg
suite byte for byte, native and wasm alike, which is the evidence that adding
the ABI did not change emulation. [Test results](test_results.md) are upstream's
and still hold.

## License

MIT, unchanged from upstream. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

    Copyright (c) 2016 Ben Smith
