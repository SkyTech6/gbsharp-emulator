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
#define GBSHARP_ABI_VERSION 2

#define GBSHARP_SCREEN_WIDTH 160
#define GBSHARP_SCREEN_HEIGHT 144

/* Samples per second per channel, and the channel count, of gbsharp_get_audio. */
#define GBSHARP_AUDIO_FREQUENCY 44100
#define GBSHARP_AUDIO_CHANNELS 2

typedef struct gbsharp_emulator gbsharp_emulator;

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
 */
GBSHARP_API void gbsharp_run_frame(gbsharp_emulator*);

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
