/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * The GB# runtime ABI.
 *
 * This is the only header GB# includes, and the only surface the shared
 * library exports. Nothing from emulator.h appears here, so the emulator
 * underneath stays replaceable: `gbsharp_emulator` is opaque and every type
 * crossing the boundary is a fixed width integer.
 *
 * Three rules shape the rest of it.
 *
 * A ROM arrives as bytes, never as a path. The core does not open files, so
 * storage policy belongs to whoever is hosting: a native player, a browser, or
 * a test that never writes anything to disk.
 *
 * gbsharp_run_frame consults no clock. It advances a fixed number of emulated
 * ticks and returns, so pacing is the host's problem. The display drives it in
 * the player, and a test drives it as fast as the CPU allows and gets the same
 * answer every time.
 *
 * Audio is pulled, not pushed. The host asks for samples after a frame rather
 * than being called back from a device thread, because a callback would tie
 * emulation progress to a sound card and take determinism with it.
 */
#ifndef GBSHARP_H_
#define GBSHARP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(GBSHARP_BUILD_SHARED)
#  if defined(_WIN32)
#    define GBSHARP_API __declspec(dllexport)
#  else
#    define GBSHARP_API __attribute__((visibility("default")))
#  endif
#else
#  define GBSHARP_API
#endif

/*
 * Bumped on any change to the signatures, semantics or layouts below, even an
 * additive one. The C# assembly and the prebuilt native library are versioned
 * and fetched independently, so "close enough" has to be a load failure with a
 * message rather than a crash three calls later.
 */
#define GBSHARP_ABI_VERSION 4

#define GBSHARP_SCREEN_WIDTH 160
#define GBSHARP_SCREEN_HEIGHT 144

/* Samples per second per channel, and the channel count, of gbsharp_get_audio. */
#define GBSHARP_AUDIO_FREQUENCY 44100
#define GBSHARP_AUDIO_CHANNELS 2

typedef struct gbsharp_emulator gbsharp_emulator;

/*
 * Why a run stopped, as a bitmask. More than one can be set: a frame that
 * completes exactly as the deadline expires reports both.
 *
 * These mirror the core's own event values rather than including its header,
 * which is the same rule the rest of this file follows. The mirroring is
 * checked at compile time in gbsharp.c, so the two cannot drift apart quietly.
 */
typedef enum gbsharp_event {
  GBSHARP_EVENT_NEW_FRAME = 0x1,
  GBSHARP_EVENT_AUDIO_BUFFER_FULL = 0x2,
  GBSHARP_EVENT_UNTIL_TICKS = 0x4,
  GBSHARP_EVENT_BREAKPOINT = 0x8,
  GBSHARP_EVENT_INVALID_OPCODE = 0x10,
} gbsharp_event;

/*
 * What the cartridge's bytes turned out to be, as flags per ROM address.
 *
 * A byte can be both code and data, and one that is neither was never touched
 * at all — which is the flag nobody sets and the answer most worth having,
 * because it is how a ROM says which of its code a play session never reached.
 *
 * Mirrors the core's own values, checked at compile time in gbsharp.c.
 */
typedef enum gbsharp_rom_usage {
  GBSHARP_ROM_USAGE_CODE = 0x1,       /* Executed, or part of an instruction that was. */
  GBSHARP_ROM_USAGE_DATA = 0x2,       /* Read as data. */
  GBSHARP_ROM_USAGE_CODE_START = 0x4, /* The first byte of an executed instruction. */
} gbsharp_rom_usage;

/*
 * The CPU's registers, flattened.
 *
 * A struct rather than fifteen accessors because a debugger reads all of them
 * at once and a caller that read them one at a time could see a torn set. The
 * layout is fixed as part of the ABI: fields are never reordered or removed,
 * only appended, and appending bumps GBSHARP_ABI_VERSION like anything else.
 *
 * `f` is the flags register packed as the hardware packs it, ZNHC in bits 7
 * to 4, so a caller can compare it against a byte read from a stack frame.
 */
typedef struct gbsharp_registers {
  uint16_t af;
  uint16_t bc;
  uint16_t de;
  uint16_t hl;
  uint16_t sp;
  uint16_t pc;
  uint8_t a;
  uint8_t f;
} gbsharp_registers;

/* Ordered as the joypad register reads them: P14 selects the direction keys,
 * P15 the buttons, low nibble in this order within each. */
typedef enum gbsharp_button {
  GBSHARP_BUTTON_RIGHT = 0,
  GBSHARP_BUTTON_LEFT = 1,
  GBSHARP_BUTTON_UP = 2,
  GBSHARP_BUTTON_DOWN = 3,
  GBSHARP_BUTTON_A = 4,
  GBSHARP_BUTTON_B = 5,
  GBSHARP_BUTTON_SELECT = 6,
  GBSHARP_BUTTON_START = 7,
} gbsharp_button;

/* GBSHARP_ABI_VERSION as the library was built. Check it before anything else. */
GBSHARP_API uint32_t gbsharp_abi_version(void);

/*
 * True in the instrumented flavour of the library. Because emulator-debug.c
 * compiles emulator.c a second time with hooks enabled, instrumentation is a
 * build time choice rather than a runtime one, so the two flavours ship as
 * separate files exporting this identical ABI. Debug only entry points return
 * false or zero in the fast flavour rather than being absent from it, which is
 * what lets one set of P/Invoke declarations serve both.
 */
GBSHARP_API bool gbsharp_has_debug_support(void);

/* An emulator with no cartridge in it. Call gbsharp_load_rom next. */
GBSHARP_API gbsharp_emulator* gbsharp_create(void);
GBSHARP_API void gbsharp_destroy(gbsharp_emulator*);

/*
 * Copies the ROM in and boots it. The caller's bytes are not retained, and a
 * second call replaces the cartridge, discarding all machine state.
 *
 * Returns false when the bytes cannot be mapped as a cartridge at all: empty,
 * larger than 8MB, or declaring a RAM size that no cartridge has.
 *
 * That is a low bar, and knowing where it sits matters more than wishing it
 * were higher. The Nintendo logo and the header checksums are not checked,
 * although a real boot ROM would check them, because homebrew and test ROMs
 * with imperfect headers are exactly what an emulator is most needed for. A
 * cartridge type byte that names no known mapper is treated as having no
 * mapper rather than refused, for the same reason. A caller that wants boot ROM
 * strictness has to check the header itself.
 *
 * Save RAM is left empty, so restore it with gbsharp_write_save_ram after
 * loading rather than before.
 */
GBSHARP_API bool gbsharp_load_rom(gbsharp_emulator*, const uint8_t* rom,
                                  size_t size);

/*
 * Back to the state gbsharp_load_rom produced, deterministically: same RAM
 * seed, same registers, same tick count. Held buttons stay held, since the
 * host's idea of what the user is pressing did not change.
 *
 * Invalidates the pointer from gbsharp_get_framebuffer.
 */
GBSHARP_API void gbsharp_reset(gbsharp_emulator*);

/*
 * Advances one frame and returns with a complete frame in the framebuffer.
 *
 * "One frame" is 70224 ticks, which is what the PPU takes to produce one,
 * measured against an absolute deadline so that N calls advance N frames with
 * no accumulated drift. The call then runs on to the end of the frame the PPU
 * is drawing, which is under one frame away, so that the framebuffer never
 * holds the top of a new frame above the bottom of the old one.
 *
 * A ROM may turn the display off, in which case no frame is ever completed.
 * That is bounded rather than waited on: the call gives up after one extra
 * frame's ticks and leaves the framebuffer as the PPU last left it.
 *
 * Returns why it stopped, as gbsharp_event flags. A player can ignore it; a
 * caller with breakpoints set cannot, because a frame that stopped early
 * stopped somewhere, and the framebuffer is then a partial frame.
 */
GBSHARP_API uint32_t gbsharp_run_frame(gbsharp_emulator*);

/*
 * GBSHARP_SCREEN_WIDTH * GBSHARP_SCREEN_HEIGHT pixels, row major from the top
 * left. Each pixel is 0xAABBGGRR, which is R, G, B, A in ascending byte order
 * on a little endian host.
 *
 * Points into the emulator and stays valid until gbsharp_reset,
 * gbsharp_load_rom or gbsharp_destroy, so do not cache it across those. The
 * contents are overwritten by gbsharp_run_frame; copy if you need to keep a
 * frame.
 */
GBSHARP_API const uint32_t* gbsharp_get_framebuffer(gbsharp_emulator*);

/*
 * Drains up to `frames` frames of audio produced by the last gbsharp_run_frame
 * into `dst`, returning how many it wrote. A frame is one sample per channel,
 * so `dst` must hold frames * GBSHARP_AUDIO_CHANNELS samples, interleaved left
 * then right, signed 16 bit at GBSHARP_AUDIO_FREQUENCY.
 *
 * One frame of video holds about 736 frames of audio. Anything not drained
 * before the next gbsharp_run_frame is discarded rather than queued: a host
 * that falls behind should hear a gap, not watch emulation stall to keep its
 * buffer intact.
 */
GBSHARP_API size_t gbsharp_get_audio(gbsharp_emulator*, int16_t* dst,
                                     size_t frames);

/*
 * Presses or releases a button, taking effect at the next gbsharp_run_frame.
 * Buttons are held until released, and survive gbsharp_reset.
 */
GBSHARP_API void gbsharp_set_button(gbsharp_emulator*, gbsharp_button,
                                    bool pressed);

/*
 * Reads and writes through the memory map as a debugger would: the current
 * bank at 0x4000, no bus timing, and no side effect on the emulated clock.
 *
 * Writing to an address the cartridge maps to its MBC changes banks, exactly
 * as a write from emulated code would.
 */
GBSHARP_API uint8_t gbsharp_read_memory(gbsharp_emulator*, uint16_t address);
GBSHARP_API void gbsharp_write_memory(gbsharp_emulator*, uint16_t address,
                                      uint8_t value);

/*
 * Where the CPU is about to execute, and which ROM bank is mapped under a
 * given address.
 *
 * Together these name a location the way a linker's `.sym` file does — bank
 * and address — which is what lets a host turn a running program counter back
 * into the symbol, and then the source line, that produced it.
 *
 * gbsharp_get_rom_bank answers for the region containing `address`: the fixed
 * region below 0x4000, or the switchable one from 0x4000 to 0x7fff. It returns
 * -1, never a bank number, when `address` is above the cartridge or when no
 * cartridge is loaded, because bank 0 is a real answer and "there is no bank
 * here" is not. Code does run outside the cartridge — a copy routine in HRAM,
 * say — so this is a case a caller meets rather than a defensive one.
 *
 * Neither needs the instrumented flavour: the state they read is kept by the
 * emulator core in every build. That is deliberate, because naming the code
 * you are running should not cost the speed of running it.
 */
GBSHARP_API uint16_t gbsharp_get_pc(gbsharp_emulator*);
GBSHARP_API int32_t gbsharp_get_rom_bank(gbsharp_emulator*, uint16_t address);

/*
 * The cartridge RAM bank currently mapped at 0xa000, or -1 when the cartridge
 * has no RAM. With the bank at 0x4000 this is the whole of the MBC state a
 * caller can act on; everything else an MBC holds is latched input to these
 * two.
 */
GBSHARP_API int32_t gbsharp_get_ram_bank(gbsharp_emulator*);

/* Bytes of cartridge ROM, after the padding gbsharp_load_rom applied. This is
 * how many entries gbsharp_read_profile can fill. */
GBSHARP_API size_t gbsharp_get_rom_size(gbsharp_emulator*);

/*
 * All the registers at one instant. Returns false, leaving `out` untouched,
 * when there is no cartridge or the library is the fast flavour.
 *
 * The whole set is read at once because a caller that read them one at a time
 * could see a torn set, and because a register dump is what a caller wants.
 */
GBSHARP_API bool gbsharp_get_registers(gbsharp_emulator*, gbsharp_registers* out);

/*
 * Executes one instruction and returns why it stopped, as gbsharp_event flags.
 *
 * Unlike the rest of the debug surface this works in both flavours, because
 * the core's stepping is in emulator.c. Breakpoints are what need the hooks,
 * so in the fast flavour a step is simply a step.
 *
 * The framebuffer is not complete after a step. Read it only after a run that
 * reported GBSHARP_EVENT_NEW_FRAME.
 */
GBSHARP_API uint32_t gbsharp_step(gbsharp_emulator*);

/*
 * Breaks at `address` in ROM bank `bank`, returning an id to remove it with,
 * or -1 when there is no room or no instrumentation.
 *
 * The bank is given rather than inferred, so a breakpoint can be set on code
 * in a bank that is not currently mapped — which is the normal case, because a
 * caller placing one from a linker map has not run the code yet. For an
 * address outside the cartridge the bank is ignored.
 *
 * Breakpoints belong to the library rather than to an emulator, because that
 * is where the core keeps them. A process running two emulators shares them.
 */
GBSHARP_API int32_t gbsharp_add_breakpoint(gbsharp_emulator*, uint16_t bank,
                                           uint16_t address);
GBSHARP_API void gbsharp_remove_breakpoint(int32_t id);
GBSHARP_API void gbsharp_clear_breakpoints(void);

/*
 * Per address execution counts and tick costs for the cartridge, gathered
 * while profiling is enabled. Both are zero in the fast flavour, where
 * enabling profiling does nothing and reports that it did nothing.
 *
 * Profiling is off by default and costs something to leave on, so it is a
 * switch rather than a permanent expense.
 */
GBSHARP_API bool gbsharp_set_profiling_enabled(bool enabled);
GBSHARP_API bool gbsharp_get_profiling_enabled(void);
GBSHARP_API void gbsharp_clear_profile(void);

/*
 * Copies profile data for the first `entries` ROM addresses, returning how
 * many it wrote — zero without instrumentation. Either destination may be
 * null to ask for only the other.
 *
 * Indexed by ROM address, meaning the offset into the cartridge file, which is
 * bank * 0x4000 + (address & 0x3fff). That is the same coordinate a linker map
 * uses, so a caller can add these up per symbol without knowing anything about
 * the emulator.
 *
 * `counts` says how often the instruction at an address ran. `cycles` says how
 * many ticks were spent there, measured to the start of the next instruction,
 * so interrupt dispatch is billed to the instruction it interrupted. The two
 * rank code differently and the difference is the point: a rarely called
 * routine can dominate a frame.
 */
GBSHARP_API size_t gbsharp_read_profile(gbsharp_emulator*, uint32_t* counts,
                                        uint32_t* cycles, size_t entries);

/*
 * Which of the cartridge's bytes were reached, as gbsharp_rom_usage flags per
 * ROM address. Zero everywhere in the fast flavour.
 *
 * Unlike profiling this is on by default, because it is what the core does
 * anyway in the hook it is already running, and because the question it
 * answers — what did this session never touch — is one a caller cannot ask
 * retroactively if nobody was recording.
 *
 * Reading and clearing both require it to be on, which is the core's own
 * precondition rather than one added here.
 */
GBSHARP_API bool gbsharp_set_rom_usage_enabled(bool enabled);
GBSHARP_API bool gbsharp_get_rom_usage_enabled(void);
GBSHARP_API void gbsharp_clear_rom_usage(void);
GBSHARP_API size_t gbsharp_read_rom_usage(gbsharp_emulator*, uint8_t* usage,
                                          size_t entries);

/*
 * Bytes of battery backed cartridge RAM, or zero when the cartridge has no
 * battery. A cartridge can have RAM without a battery, in which case there is
 * nothing worth persisting and this reports zero.
 *
 * gbsharp_read_save_ram fills `destination` with that many bytes;
 * gbsharp_write_save_ram loads that many back. Both no-op when the size is
 * zero, so a host can persist saves without asking what cartridge it has.
 */
GBSHARP_API size_t gbsharp_save_ram_size(gbsharp_emulator*);
GBSHARP_API void gbsharp_read_save_ram(gbsharp_emulator*, uint8_t* destination);
GBSHARP_API void gbsharp_write_save_ram(gbsharp_emulator*, const uint8_t* source);

#ifdef __cplusplus
}
#endif

#endif /* GBSHARP_H_ */
