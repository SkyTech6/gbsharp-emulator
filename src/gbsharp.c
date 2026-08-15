/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include "gbsharp.h"

#include "emulator.h"
#include "memory.h"

#if defined(GBSHARP_DEBUG_FLAVOUR)
#include "emulator-debug.h"
#endif

/*
 * gbsharp.h mirrors the core's event values rather than including its header.
 * Mirrored constants drift; these cannot, because the build stops if they do.
 */
#define GBSHARP_STATIC_ASSERT(x) _Static_assert(x, #x)

GBSHARP_STATIC_ASSERT((int)GBSHARP_EVENT_NEW_FRAME == EMULATOR_EVENT_NEW_FRAME);
GBSHARP_STATIC_ASSERT((int)GBSHARP_EVENT_AUDIO_BUFFER_FULL ==
                      EMULATOR_EVENT_AUDIO_BUFFER_FULL);
GBSHARP_STATIC_ASSERT((int)GBSHARP_EVENT_UNTIL_TICKS ==
                      EMULATOR_EVENT_UNTIL_TICKS);
GBSHARP_STATIC_ASSERT((int)GBSHARP_EVENT_BREAKPOINT ==
                      EMULATOR_EVENT_BREAKPOINT);
GBSHARP_STATIC_ASSERT((int)GBSHARP_EVENT_INVALID_OPCODE ==
                      EMULATOR_EVENT_INVALID_OPCODE);

#if defined(GBSHARP_DEBUG_FLAVOUR)
GBSHARP_STATIC_ASSERT((int)GBSHARP_ROM_USAGE_CODE == ROM_USAGE_CODE);
GBSHARP_STATIC_ASSERT((int)GBSHARP_ROM_USAGE_DATA == ROM_USAGE_DATA);
GBSHARP_STATIC_ASSERT((int)GBSHARP_ROM_USAGE_CODE_START == ROM_USAGE_CODE_START);
#endif

/*
 * How much audio the core is allowed to buffer, in frames of one sample per
 * channel. One video frame is PPU_FRAME_TICKS worth, which at 44100Hz is a
 * little over 736 frames of audio, so a tenth of a second leaves room for six
 * video frames.
 *
 * The headroom matters because emulator_run_until stops early when the audio
 * buffer fills, and the call after that one empties it. A buffer that could
 * fill inside a single video frame would therefore lose the start of that
 * frame's audio. This is the same figure upstream's tester.c uses.
 */
#define GBSHARP_AUDIO_BUFFER_FRAMES (GBSHARP_AUDIO_FREQUENCY / 10)

/*
 * Seed for the pseudo-random contents of uninitialised RAM. Fixed, because the
 * ABI promises that two runs of the same ROM behave identically. Zero is the
 * value upstream's own test suite runs with, which is what lets GB# assert
 * against the screen hashes in binjgb's scripts/test.json.
 */
#define GBSHARP_RANDOM_SEED 0

struct gbsharp_emulator {
  Emulator* core;

  /*
   * Our own copy of the cartridge, already padded to a size the core accepts.
   * The core takes ownership of whatever buffer it is handed and may reallocate
   * it, so reset hands it a fresh duplicate of this one rather than the same
   * pointer twice.
   */
  u8* rom;
  size_t rom_size;

  /* What the host says is held down, which outlives any one Emulator. */
  JoypadButtons buttons;

  /* Absolute tick count that the next run_frame advances to. */
  Ticks deadline;

  /* Frames of the current frame's audio already handed to the host. */
  size_t audio_taken;
};

static Emulator* core_new(const u8* rom, size_t rom_size) {
  EmulatorInit init;
  ZERO_MEMORY(init);

  init.rom.size = rom_size;
  init.rom.data = xmalloc(rom_size);
  if (init.rom.data == NULL) {
    return NULL;
  }
  memcpy(init.rom.data, rom, rom_size);

  init.audio_frequency = GBSHARP_AUDIO_FREQUENCY;
  init.audio_frames = GBSHARP_AUDIO_BUFFER_FRAMES;
  init.random_seed = GBSHARP_RANDOM_SEED;
  init.builtin_palette = 0;
  init.force_dmg = FALSE;
  init.cgb_color_curve = CGB_COLOR_CURVE_NONE;
  init.quiet_cart_info = TRUE;

  /*
   * emulator_new takes the ROM buffer whether it succeeds or fails, because
   * the size was validated before we got here, so there is nothing to free on
   * this side either way.
   */
  return emulator_new(&init);
}

static void apply_buttons(gbsharp_emulator* e) {
  if (e->core != NULL) {
    emulator_set_joypad_buttons(e->core, &e->buttons);
  }
}

uint32_t gbsharp_abi_version(void) {
  return GBSHARP_ABI_VERSION;
}

bool gbsharp_has_debug_support(void) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  return true;
#else
  return false;
#endif
}

gbsharp_emulator* gbsharp_create(void) {
  return (gbsharp_emulator*)xcalloc(1, sizeof(gbsharp_emulator));
}

void gbsharp_destroy(gbsharp_emulator* e) {
  if (e == NULL) {
    return;
  }
  if (e->core != NULL) {
    emulator_delete(e->core);
  }
  xfree(e->rom);
  xfree(e);
}

bool gbsharp_load_rom(gbsharp_emulator* e, const uint8_t* rom, size_t size) {
  if (e == NULL || rom == NULL || size == 0 || size > MAXIMUM_ROM_SIZE) {
    return false;
  }

  /*
   * The core insists on a whole number of 32KB banks and reads the padding as
   * cartridge space, so pad with zero the way file_read_aligned does. A ROM
   * that arrives already aligned is copied byte for byte.
   */
  size_t padded_size = ALIGN_UP(size, MINIMUM_ROM_SIZE);
  u8* padded = (u8*)xcalloc(1, padded_size);
  if (padded == NULL) {
    return false;
  }
  memcpy(padded, rom, size);

  Emulator* core = core_new(padded, padded_size);
  if (core == NULL) {
    xfree(padded);
    return false;
  }

  if (e->core != NULL) {
    emulator_delete(e->core);
  }
  xfree(e->rom);

  e->core = core;
  e->rom = padded;
  e->rom_size = padded_size;
  e->deadline = emulator_get_ticks(core);
  e->audio_taken = 0;
  apply_buttons(e);
  return true;
}

void gbsharp_reset(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL) {
    return;
  }

  /*
   * Upstream has no reset, so this builds a new machine over the same
   * cartridge. That is the deterministic reading of reset anyway: same RAM
   * seed, same registers, same tick count as the first boot, with nothing
   * carried over that a power cycle would not carry.
   */
  Emulator* core = core_new(e->rom, e->rom_size);
  if (core == NULL) {
    return;
  }

  emulator_delete(e->core);
  e->core = core;
  e->deadline = emulator_get_ticks(core);
  e->audio_taken = 0;
  apply_buttons(e);
}

uint32_t gbsharp_run_frame(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL) {
    return 0;
  }

  /* This frame's audio starts empty. Whatever the host did not pull from the
   * last frame is gone, which is the pull model's whole point: the emulator
   * does not wait for a consumer. */
  AudioBuffer* audio = emulator_get_audio_buffer(e->core);
  audio->position = audio->data;
  e->audio_taken = 0;

  /*
   * Two things have to be true at once, and they are not the same thing.
   *
   * The clock must advance one frame's worth of ticks per call, so that N calls
   * are N frames of emulated time. The deadline is absolute rather than
   * measured from where this call started, which is what stops a few ticks of
   * overshoot per frame from accumulating into a drift.
   *
   * The framebuffer must hold a whole frame when the call returns. The PPU
   * writes it a line at a time, so stopping at an arbitrary tick leaves the
   * top of a new frame above the bottom of the old one, which is tearing.
   *
   * When the display has been off and on again the PPU is no longer in phase
   * with the tick deadline, and the two requirements pull apart. So: run to the
   * deadline, then keep going to the next completed frame. Frames are
   * PPU_FRAME_TICKS apart, so that second stretch is under one frame long and
   * the average stays at exactly one frame per call.
   */
  e->deadline += PPU_FRAME_TICKS;

  /*
   * With the display off no frame is ever completed, and upstream's tester.c
   * would spin here forever waiting for one. One frame of slack is the whole
   * allowance: after that the display is off, the framebuffer is whatever the
   * PPU last left in it, and that is the correct answer rather than a hang.
   */
  Ticks give_up_at = e->deadline + PPU_FRAME_TICKS;
  Bool past_deadline = FALSE;

  for (;;) {
    EmulatorEvent event =
        emulator_run_until(e->core, past_deadline ? give_up_at : e->deadline);

    /*
     * A breakpoint is the one event that must not be looped past. Everything
     * else here is the frame loop making progress; this is the caller asking
     * to be let out of it, and running on would step over the instruction
     * they stopped at.
     */
    if (event & EMULATOR_EVENT_BREAKPOINT) {
      /* The frame this call was running is not finished, so the tick budget
       * for it has not been spent. Handing the deadline back means the next
       * call resumes this frame rather than starting the next one and running
       * two frames' worth of ticks to catch up. */
      e->deadline -= PPU_FRAME_TICKS;
      return (uint32_t)event;
    }

    if (past_deadline && (event & EMULATOR_EVENT_NEW_FRAME)) {
      return (uint32_t)event;
    }

    if (event & EMULATOR_EVENT_UNTIL_TICKS) {
      if (past_deadline) {
        return (uint32_t)event;
      }
      past_deadline = TRUE;
    }

    /*
     * Every other event, a full audio buffer or an invalid opcode, has still
     * advanced the clock, so looping makes progress and none of them needs a
     * case of its own.
     */
  }
}

const uint32_t* gbsharp_get_framebuffer(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL) {
    return NULL;
  }
  return (const uint32_t*)*emulator_get_frame_buffer(e->core);
}

size_t gbsharp_get_audio(gbsharp_emulator* e, int16_t* dst, size_t frames) {
  if (e == NULL || e->core == NULL || dst == NULL || frames == 0) {
    return 0;
  }

  AudioBuffer* audio = emulator_get_audio_buffer(e->core);
  size_t available = audio_buffer_get_frames(audio);
  if (available <= e->audio_taken) {
    return 0;
  }

  size_t count = MIN(frames, available - e->audio_taken);
  const u8* src = audio->data + e->audio_taken * SOUND_OUTPUT_COUNT;
  size_t samples = count * SOUND_OUTPUT_COUNT;

  /* The core mixes to unsigned 8 bit centred on 128. Signed 16 bit is what
   * every host audio API we target wants, and the conversion is exact. */
  for (size_t i = 0; i < samples; ++i) {
    dst[i] = (int16_t)(((int)src[i] - 128) * 256);
  }

  e->audio_taken += count;
  return count;
}

void gbsharp_set_button(gbsharp_emulator* e, gbsharp_button button,
                        bool pressed) {
  if (e == NULL) {
    return;
  }

  Bool value = pressed ? TRUE : FALSE;
  switch (button) {
    case GBSHARP_BUTTON_RIGHT:  e->buttons.right = value; break;
    case GBSHARP_BUTTON_LEFT:   e->buttons.left = value; break;
    case GBSHARP_BUTTON_UP:     e->buttons.up = value; break;
    case GBSHARP_BUTTON_DOWN:   e->buttons.down = value; break;
    case GBSHARP_BUTTON_A:      e->buttons.A = value; break;
    case GBSHARP_BUTTON_B:      e->buttons.B = value; break;
    case GBSHARP_BUTTON_SELECT: e->buttons.select = value; break;
    case GBSHARP_BUTTON_START:  e->buttons.start = value; break;
    default: return;
  }

  apply_buttons(e);
}

uint8_t gbsharp_read_memory(gbsharp_emulator* e, uint16_t address) {
  if (e == NULL || e->core == NULL) {
    return 0;
  }
  return emulator_read_mem(e->core, address);
}

void gbsharp_write_memory(gbsharp_emulator* e, uint16_t address,
                          uint8_t value) {
  if (e == NULL || e->core == NULL) {
    return;
  }
  emulator_write_mem(e->core, address, value);
}

uint16_t gbsharp_get_pc(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL) {
    return 0;
  }
  return emulator_get_PC(e->core);
}

int32_t gbsharp_get_rom_bank(gbsharp_emulator* e, uint16_t address) {
  if (e == NULL || e->core == NULL) {
    return -1;
  }
  /* The core already answers this for an address, including the -1 for one
   * that is not in the cartridge, so the facade only forwards it. */
  return emulator_get_rom_bank(e->core, address);
}

int32_t gbsharp_get_ram_bank(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL || emulator_get_ext_ram_size(e->core) == 0) {
    return -1;
  }
  return emulator_get_ext_ram_bank(e->core);
}

size_t gbsharp_get_rom_size(gbsharp_emulator* e) {
  return e == NULL ? 0 : e->rom_size;
}

uint32_t gbsharp_step(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL) {
    return 0;
  }
  return (uint32_t)emulator_step(e->core);
}

bool gbsharp_get_registers(gbsharp_emulator* e, gbsharp_registers* out) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  if (e == NULL || e->core == NULL || out == NULL) {
    return false;
  }

  Registers r = emulator_get_registers(e->core);

  /* The core keeps the flags unpacked, one Bool each. The hardware packs them
   * into the top nibble of F, and that is the form a caller can compare
   * against a byte pushed on the stack, so pack them here. */
  uint8_t f = (uint8_t)((r.F.Z ? 0x80 : 0) | (r.F.N ? 0x40 : 0) |
                        (r.F.H ? 0x20 : 0) | (r.F.C ? 0x10 : 0));

  out->a = r.A;
  out->f = f;
  out->af = (uint16_t)((r.A << 8) | f);
  out->bc = r.BC;
  out->de = r.DE;
  out->hl = r.HL;
  out->sp = r.SP;
  out->pc = r.PC;
  return true;
#else
  (void)e;
  (void)out;
  return false;
#endif
}

int32_t gbsharp_add_breakpoint(gbsharp_emulator* e, uint16_t bank,
                               uint16_t address) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  if (e == NULL || e->core == NULL) {
    return -1;
  }

  int id = emulator_add_breakpoint(e->core, address, TRUE);
  if (id < 0) {
    return -1;
  }

  /* emulator_add_breakpoint records whichever bank happens to be mapped now.
   * A caller placing a breakpoint from a linker map has not run the code yet,
   * so the bank it wants is almost never the one that is mapped. */
  emulator_set_breakpoint_bank(id, (u8)bank);
  return id;
#else
  (void)e;
  (void)bank;
  (void)address;
  return -1;
#endif
}

void gbsharp_remove_breakpoint(int32_t id) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  if (id >= 0) {
    emulator_remove_breakpoint((int)id);
  }
#else
  (void)id;
#endif
}

void gbsharp_clear_breakpoints(void) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  /* Downwards: removing the highest id is what lets the core shrink its own
   * high water mark, and removing an invalid id is a no-op either way. */
  for (int id = emulator_get_max_breakpoint_id() - 1; id >= 0; --id) {
    emulator_remove_breakpoint(id);
  }
#endif
}

bool gbsharp_set_profiling_enabled(bool enabled) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  emulator_set_profiling_enabled(enabled ? TRUE : FALSE);
  return enabled;
#else
  /* Reporting the state that was actually reached, which without hooks is
   * always off. A caller that believed otherwise would read zeroes and
   * conclude its game executes no code. */
  (void)enabled;
  return false;
#endif
}

bool gbsharp_get_profiling_enabled(void) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  return emulator_get_profiling_enabled() == TRUE;
#else
  return false;
#endif
}

void gbsharp_clear_profile(void) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  emulator_clear_profiling_counters();
#endif
}

size_t gbsharp_read_profile(gbsharp_emulator* e, uint32_t* counts,
                            uint32_t* cycles, size_t entries) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  if (e == NULL || e->core == NULL || entries == 0) {
    return 0;
  }

  /* The core's arrays are MAXIMUM_ROM_SIZE regardless of the cartridge, so
   * the cartridge is what bounds the copy. Past it every entry is zero and
   * copying them would only be slower. */
  size_t count = MIN(entries, e->rom_size);

  if (counts != NULL) {
    memcpy(counts, emulator_get_profiling_counters(), count * sizeof(uint32_t));
  }
  if (cycles != NULL) {
    memcpy(cycles, emulator_get_profiling_cycles(), count * sizeof(uint32_t));
  }
  return count;
#else
  (void)e;
  (void)counts;
  (void)cycles;
  (void)entries;
  return 0;
#endif
}

bool gbsharp_set_rom_usage_enabled(bool enabled) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  emulator_set_rom_usage_enabled(enabled ? TRUE : FALSE);
  return enabled;
#else
  (void)enabled;
  return false;
#endif
}

bool gbsharp_get_rom_usage_enabled(void) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  return emulator_get_rom_usage_enabled() == TRUE;
#else
  return false;
#endif
}

void gbsharp_clear_rom_usage(void) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  /* The core asserts on this rather than checking it, so the check is here. */
  if (emulator_get_rom_usage_enabled()) {
    emulator_clear_rom_usage();
  }
#endif
}

size_t gbsharp_read_rom_usage(gbsharp_emulator* e, uint8_t* usage,
                              size_t entries) {
#if defined(GBSHARP_DEBUG_FLAVOUR)
  if (e == NULL || e->core == NULL || usage == NULL || entries == 0 ||
      !emulator_get_rom_usage_enabled()) {
    return 0;
  }

  size_t count = MIN(entries, e->rom_size);
  memcpy(usage, emulator_get_rom_usage(), count);
  return count;
#else
  (void)e;
  (void)usage;
  (void)entries;
  return 0;
#endif
}

size_t gbsharp_save_ram_size(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL || !emulator_has_battery(e->core)) {
    return 0;
  }
  return emulator_get_ext_ram_size(e->core);
}

void gbsharp_read_save_ram(gbsharp_emulator* e, uint8_t* destination) {
  size_t size = gbsharp_save_ram_size(e);
  if (size == 0 || destination == NULL) {
    return;
  }

  FileData file_data;
  file_data.data = destination;
  file_data.size = size;
  emulator_write_ext_ram(e->core, &file_data);
}

void gbsharp_write_save_ram(gbsharp_emulator* e, const uint8_t* source) {
  size_t size = gbsharp_save_ram_size(e);
  if (size == 0 || source == NULL) {
    return;
  }

  FileData file_data;
  file_data.data = (u8*)source;
  file_data.size = size;
  emulator_read_ext_ram(e->core, &file_data);
}
