/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include "gbsharp.h"

#include "emulator.h"
#include "memory.h"

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

void gbsharp_run_frame(gbsharp_emulator* e) {
  if (e == NULL || e->core == NULL) {
    return;
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

    if (past_deadline && (event & EMULATOR_EVENT_NEW_FRAME)) {
      break;
    }

    if (event & EMULATOR_EVENT_UNTIL_TICKS) {
      if (past_deadline) {
        break;
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
