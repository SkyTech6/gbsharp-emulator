/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * The GB# runtime ABI, in a browser.
 *
 * Every method here is one entry point of gbsharp.h under the same name, in the
 * same order, with the same meaning. That is the whole point of the file: a
 * host written against the native ABI and a host written against this one are
 * the same program, and somebody reading either can follow the other.
 *
 * What it does not do is decide anything. Pacing, input mapping, audio devices,
 * scaling and save storage are the host's business, exactly as they are in C,
 * and they live in the web player rather than here.
 */

const SCREEN_WIDTH = 160;
const SCREEN_HEIGHT = 144;
const AUDIO_FREQUENCY = 44100;
const AUDIO_CHANNELS = 2;
const ABI_VERSION = 4;

/* Audio frames pulled per call. One video frame is about 736 of them. */
const AUDIO_PULL_FRAMES = 2048;

export class GameBoy {
  /*
   * Instantiates the wasm module and checks that it speaks the version this
   * file was written against, for the same reason the C# side does: the module
   * and its wrapper are separately cached by browsers, and a mismatch has to be
   * a message rather than a wrong answer three calls later.
   */
  static async create(factory, options = {}) {
    const module = await factory(options);

    const version = module._gbsharp_abi_version();
    if (version !== ABI_VERSION) {
      throw new Error(
        `The GB# runtime reports ABI version ${version}, but this player ` +
        `speaks version ${ABI_VERSION}.`);
    }

    return new GameBoy(module);
  }

  constructor(module) {
    this.module = module;
    this.pointer = module._gbsharp_create();

    if (this.pointer === 0) {
      throw new Error('The GB# runtime could not create an emulator.');
    }

    /* Scratch space for pulled audio, allocated once. */
    this.audioBytes = AUDIO_PULL_FRAMES * AUDIO_CHANNELS * 2;
    this.audioBuffer = module._malloc(this.audioBytes);
  }

  destroy() {
    if (this.pointer !== 0) {
      this.module._gbsharp_destroy(this.pointer);
      this.pointer = 0;
    }
    if (this.audioBuffer !== 0) {
      this.module._free(this.audioBuffer);
      this.audioBuffer = 0;
    }
  }

  get hasDebugSupport() {
    return this.module._gbsharp_has_debug_support() !== 0;
  }

  /*
   * Copies a ROM into the module's heap and boots it. Bytes, never a path:
   * there is no filesystem in this build, which is the same constraint the
   * native ABI imposes on itself for the sake of this one.
   */
  loadRom(bytes) {
    const pointer = this.module._malloc(bytes.length);

    try {
      this.module.HEAPU8.set(bytes, pointer);
      return this.module._gbsharp_load_rom(this.pointer, pointer, bytes.length) !== 0;
    } finally {
      this.module._free(pointer);
    }
  }

  reset() {
    this.module._gbsharp_reset(this.pointer);
  }

  /* Returns why it stopped, as Event flags. A player can ignore it. */
  runFrame() {
    return this.module._gbsharp_run_frame(this.pointer);
  }

  /* Executes one instruction, returning why it stopped. */
  step() {
    return this.module._gbsharp_step(this.pointer);
  }

  /*
   * The screen as it stands, 160x144, each pixel 0xAABBGGRR.
   *
   * A view over the module's heap rather than a copy, so it costs nothing to
   * ask for and must not be kept: the next runFrame overwrites it, reset moves
   * it, and growing the heap detaches the whole buffer it points into. Fetched
   * fresh each frame for exactly those reasons.
   */
  get framebuffer() {
    const pointer = this.module._gbsharp_get_framebuffer(this.pointer);
    if (pointer === 0) {
      return null;
    }

    return new Uint32Array(
      this.module.HEAPU32.buffer, pointer, SCREEN_WIDTH * SCREEN_HEIGHT);
  }

  /*
   * Drains the audio the last runFrame produced, as interleaved signed 16 bit
   * samples. Returns a view valid until the next call, which the host copies
   * into whatever its audio device wants.
   */
  readAudio() {
    const frames = this.module._gbsharp_get_audio(
      this.pointer, this.audioBuffer, AUDIO_PULL_FRAMES);

    if (frames === 0) {
      return null;
    }

    return new Int16Array(
      this.module.HEAP16.buffer, this.audioBuffer, frames * AUDIO_CHANNELS);
  }

  setButton(button, pressed) {
    this.module._gbsharp_set_button(this.pointer, button, pressed ? 1 : 0);
  }

  readMemory(address) {
    return this.module._gbsharp_read_memory(this.pointer, address);
  }

  writeMemory(address, value) {
    this.module._gbsharp_write_memory(this.pointer, address, value);
  }

  /* Where the CPU is about to execute. */
  get programCounter() {
    return this.module._gbsharp_get_pc(this.pointer);
  }

  /* The ROM bank mapped under `address`, or zero above 0x7fff, where the
   * address is not in the cartridge at all. */
  romBankAt(address) {
    return this.module._gbsharp_get_rom_bank(this.pointer, address);
  }

  /* Bytes of battery backed cartridge RAM, or zero when nothing is worth saving. */
  get saveRamSize() {
    return this.module._gbsharp_save_ram_size(this.pointer);
  }

  readSaveRam() {
    const size = this.saveRamSize;
    if (size === 0) {
      return null;
    }

    const pointer = this.module._malloc(size);
    try {
      this.module._gbsharp_read_save_ram(this.pointer, pointer);
      /* Sliced, not viewed: this outlives the call and gets stored. */
      return this.module.HEAPU8.slice(pointer, pointer + size);
    } finally {
      this.module._free(pointer);
    }
  }

  writeSaveRam(bytes) {
    const size = this.saveRamSize;
    if (size === 0 || bytes.length < size) {
      return false;
    }

    const pointer = this.module._malloc(size);
    try {
      this.module.HEAPU8.set(bytes.subarray(0, size), pointer);
      this.module._gbsharp_write_save_ram(this.pointer, pointer);
      return true;
    } finally {
      this.module._free(pointer);
    }
  }
}

/* gbsharp_button, in the ABI's own order. */
export const Button = Object.freeze({
  Right: 0,
  Left: 1,
  Up: 2,
  Down: 3,
  A: 4,
  B: 5,
  Select: 6,
  Start: 7,
});

/* gbsharp_rom_usage, the flags in the array readRomUsage fills. A byte with
 * none of them set was never reached at all. */
export const RomUsage = Object.freeze({
  Code: 0x1,
  Data: 0x2,
  CodeStart: 0x4,
});

/* gbsharp_event, the flags runFrame and step return. */
export const Event = Object.freeze({
  NewFrame: 0x1,
  AudioBufferFull: 0x2,
  UntilTicks: 0x4,
  Breakpoint: 0x8,
  InvalidOpcode: 0x10,
});

export const Screen = Object.freeze({
  width: SCREEN_WIDTH,
  height: SCREEN_HEIGHT,
});

export const Audio = Object.freeze({
  frequency: AUDIO_FREQUENCY,
  channels: AUDIO_CHANNELS,
});

export const AbiVersion = ABI_VERSION;
