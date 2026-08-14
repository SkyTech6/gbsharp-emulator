/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * The GB# Player: a window, a sound device, a joypad, and nothing else.
 *
 * This is what a published GB# game is. It carries the ROM inside itself (see
 * payload.h), opens straight into the game, and has no emulator vocabulary
 * anywhere in it: no ROM browser, no save states, no settings, no menu. A
 * player that offered those would be an emulator that happens to have a game
 * in it, which is not what somebody who published a game wants to hand out.
 *
 * It talks to the emulator through gbsharp.h, the same ABI the test harness
 * and the web runtime use, rather than through the core's own headers. That is
 * deliberate: it means the boundary is exercised by the thing users run, and a
 * change that breaks the ABI breaks this too rather than silently working here
 * and failing everywhere else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "gbsharp.h"

#include "config.h"
#include "payload.h"

#define GB_WIDTH GBSHARP_SCREEN_WIDTH
#define GB_HEIGHT GBSHARP_SCREEN_HEIGHT

/*
 * The Game Boy's frame rate, as a period in nanoseconds: 4194304 ticks per
 * second, 70224 ticks per frame, so 59.7275Hz.
 *
 * The display does not drive the emulator, because a 144Hz display would then
 * run the game two and a half times too fast and a 50Hz one would run it slow.
 * Vsync stops the tearing; this decides when a frame is due.
 */
#define FRAME_PERIOD_NS 16742706ull

/*
 * How far behind the player is willing to catch up. A machine that stalls for
 * a second should carry on from where it is rather than fast forwarding
 * through a second of game, which is what running the missed frames would do.
 */
#define MAX_CATCHUP_FRAMES 4

#define AUDIO_BUFFER_FRAMES 2048

/*
 * Keep roughly this many frames of audio queued. Too little and the device
 * underruns into clicks; too much and the sound lags visibly behind the
 * picture. Three frames is about 50ms, which is under the threshold where a
 * player notices sound arriving late.
 */
#define AUDIO_TARGET_FRAMES (3 * (GBSHARP_AUDIO_FREQUENCY / 60))

typedef struct player {
  SDL_Window* window;
  SDL_Renderer* renderer;
  SDL_Texture* texture;
  SDL_AudioDeviceID audio;
  SDL_GameController* controller;

  gbsharp_emulator* emulator;
  gbsharp_config config;

  char* save_path;
  size_t save_size;
  uint32_t save_signature;

  bool running;
} player;

/* ------------------------------------------------------------------------- */
/* Errors                                                                     */
/*                                                                            */
/* A published game is run by somebody who has never heard of this program, so
 * a failure has to arrive as a message box rather than as a line on a stderr
 * nobody is looking at.                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Set by --frames, which runs a fixed number of frames and exits. It exists so
 * that CI can start a published game and find out whether it runs, and so that
 * the failure paths below report through an exit code rather than through a
 * dialog box that nothing is there to dismiss.
 */
static bool s_automated = false;

static void fail(const char* title, const char* message) {
  fprintf(stderr, "%s: %s\n", title, message);
  if (!s_automated) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
  }
}

/* ------------------------------------------------------------------------- */
/* Save RAM                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t signature(const uint8_t* data, size_t size) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

/*
 * Saves live in the per-user application data directory, not next to the
 * executable: a game may be installed somewhere the user cannot write, and two
 * users on one machine each have their own progress.
 */
static char* save_file_path(const char* title) {
  char* base = SDL_GetPrefPath("GBSharp", (title != NULL && title[0] != '\0')
                                              ? title
                                              : "GB# Player");
  if (base == NULL) {
    return NULL;
  }

  size_t length = strlen(base) + strlen("save.sav") + 1;
  char* path = (char*)malloc(length);
  if (path != NULL) {
    snprintf(path, length, "%ssave.sav", base);
  }
  SDL_free(base);
  return path;
}

static void save_load(player* p) {
  p->save_size = gbsharp_save_ram_size(p->emulator);
  if (p->save_size == 0 || p->save_path == NULL) {
    return;
  }

  SDL_RWops* file = SDL_RWFromFile(p->save_path, "rb");
  if (file == NULL) {
    /* No save yet, which is what a first run looks like. */
    return;
  }

  uint8_t* bytes = (uint8_t*)calloc(1, p->save_size);
  if (bytes != NULL &&
      SDL_RWread(file, bytes, 1, p->save_size) == (size_t)p->save_size) {
    gbsharp_write_save_ram(p->emulator, bytes);
    p->save_signature = signature(bytes, p->save_size);
  }

  free(bytes);
  SDL_RWclose(file);
}

/* Writes only when the contents changed, so a game with a battery does not
 * rewrite the file every second for the life of the process. */
static void save_store(player* p, bool force) {
  if (p->save_size == 0 || p->save_path == NULL) {
    return;
  }

  uint8_t* bytes = (uint8_t*)calloc(1, p->save_size);
  if (bytes == NULL) {
    return;
  }
  gbsharp_read_save_ram(p->emulator, bytes);

  uint32_t current = signature(bytes, p->save_size);
  if (!force && current == p->save_signature) {
    free(bytes);
    return;
  }

  SDL_RWops* file = SDL_RWFromFile(p->save_path, "wb");
  if (file != NULL) {
    SDL_RWwrite(file, bytes, 1, p->save_size);
    SDL_RWclose(file);
    p->save_signature = current;
  }

  free(bytes);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

static bool button_for_key(SDL_Keycode key, gbsharp_button* button) {
  switch (key) {
    case SDLK_RIGHT: *button = GBSHARP_BUTTON_RIGHT; return true;
    case SDLK_LEFT: *button = GBSHARP_BUTTON_LEFT; return true;
    case SDLK_UP: *button = GBSHARP_BUTTON_UP; return true;
    case SDLK_DOWN: *button = GBSHARP_BUTTON_DOWN; return true;
    case SDLK_x: *button = GBSHARP_BUTTON_A; return true;
    case SDLK_z: *button = GBSHARP_BUTTON_B; return true;
    case SDLK_RETURN: *button = GBSHARP_BUTTON_START; return true;
    case SDLK_BACKSPACE: *button = GBSHARP_BUTTON_SELECT; return true;
    case SDLK_RSHIFT: *button = GBSHARP_BUTTON_SELECT; return true;
    default: return false;
  }
}

static bool button_for_pad(Uint8 pad, gbsharp_button* button) {
  switch (pad) {
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: *button = GBSHARP_BUTTON_RIGHT; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: *button = GBSHARP_BUTTON_LEFT; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: *button = GBSHARP_BUTTON_UP; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: *button = GBSHARP_BUTTON_DOWN; return true;
    case SDL_CONTROLLER_BUTTON_A: *button = GBSHARP_BUTTON_A; return true;
    case SDL_CONTROLLER_BUTTON_B: *button = GBSHARP_BUTTON_B; return true;
    case SDL_CONTROLLER_BUTTON_START: *button = GBSHARP_BUTTON_START; return true;
    case SDL_CONTROLLER_BUTTON_BACK: *button = GBSHARP_BUTTON_SELECT; return true;
    default: return false;
  }
}

static void toggle_fullscreen(player* p) {
  Uint32 flags = SDL_GetWindowFlags(p->window);
  bool full = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  SDL_SetWindowFullscreen(p->window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static void handle_events(player* p) {
  SDL_Event event;
  gbsharp_button button;

  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:
        p->running = false;
        break;

      case SDL_KEYDOWN:
        /* The only two shortcuts. Alt+Enter because every game on this
         * platform has it, Escape because leaving fullscreen has to be
         * possible without knowing anything. */
        if (event.key.keysym.sym == SDLK_RETURN &&
            (event.key.keysym.mod & KMOD_ALT) != 0) {
          toggle_fullscreen(p);
          break;
        }
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          if ((SDL_GetWindowFlags(p->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
            SDL_SetWindowFullscreen(p->window, 0);
          } else {
            p->running = false;
          }
          break;
        }
        if (event.key.repeat == 0 && button_for_key(event.key.keysym.sym, &button)) {
          gbsharp_set_button(p->emulator, button, true);
        }
        break;

      case SDL_KEYUP:
        if (button_for_key(event.key.keysym.sym, &button)) {
          gbsharp_set_button(p->emulator, button, false);
        }
        break;

      case SDL_CONTROLLERBUTTONDOWN:
        if (button_for_pad(event.cbutton.button, &button)) {
          gbsharp_set_button(p->emulator, button, true);
        }
        break;

      case SDL_CONTROLLERBUTTONUP:
        if (button_for_pad(event.cbutton.button, &button)) {
          gbsharp_set_button(p->emulator, button, false);
        }
        break;

      case SDL_CONTROLLERDEVICEADDED:
        if (p->controller == NULL) {
          p->controller = SDL_GameControllerOpen(event.cdevice.which);
        }
        break;

      case SDL_CONTROLLERDEVICEREMOVED:
        if (p->controller != NULL &&
            SDL_GameControllerFromInstanceID(event.cdevice.which) == p->controller) {
          SDL_GameControllerClose(p->controller);
          p->controller = NULL;
        }
        break;

      default:
        break;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Presentation                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Where the 160x144 picture goes inside whatever size the window is now.
 *
 * Integer scaling keeps every pixel the same size as every other pixel, which
 * is the difference between a Game Boy screen and a photograph of one. It costs
 * some black border, and the alternative costs a shimmering, unevenly weighted
 * grid, so it is the default.
 */
static SDL_Rect present_rect(const player* p, int width, int height) {
  SDL_Rect rect;

  if (p->config.integer_scaling) {
    int scale = width / GB_WIDTH;
    int vertical = height / GB_HEIGHT;
    if (vertical < scale) {
      scale = vertical;
    }
    if (scale < 1) {
      scale = 1;
    }
    rect.w = GB_WIDTH * scale;
    rect.h = GB_HEIGHT * scale;
  } else {
    /* Fill, but keep the aspect ratio: a stretched Game Boy looks wrong in a
     * way people notice without being able to say why. */
    int by_width = width;
    int by_height = (width * GB_HEIGHT) / GB_WIDTH;
    if (by_height > height) {
      by_height = height;
      by_width = (height * GB_WIDTH) / GB_HEIGHT;
    }
    rect.w = by_width;
    rect.h = by_height;
  }

  rect.x = (width - rect.w) / 2;
  rect.y = (height - rect.h) / 2;
  return rect;
}

static void present(player* p) {
  const uint32_t* pixels = gbsharp_get_framebuffer(p->emulator);
  if (pixels == NULL) {
    return;
  }

  SDL_UpdateTexture(p->texture, NULL, pixels, GB_WIDTH * (int)sizeof(uint32_t));

  int width = 0, height = 0;
  SDL_GetRendererOutputSize(p->renderer, &width, &height);

  SDL_Rect rect = present_rect(p, width, height);

  SDL_SetRenderDrawColor(p->renderer, 0, 0, 0, 255);
  SDL_RenderClear(p->renderer);
  SDL_RenderCopy(p->renderer, p->texture, NULL, &rect);
  SDL_RenderPresent(p->renderer);
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */

static void pump_audio(player* p) {
  static int16_t samples[AUDIO_BUFFER_FRAMES * GBSHARP_AUDIO_CHANNELS];

  size_t frames = gbsharp_get_audio(p->emulator, samples, AUDIO_BUFFER_FRAMES);
  if (frames == 0 || p->audio == 0) {
    return;
  }

  /* Volume is applied here rather than by the emulator, because it is a
   * property of this speaker and not of the machine. */
  if (p->config.volume < 100) {
    int scale = p->config.volume;
    for (size_t i = 0; i < frames * GBSHARP_AUDIO_CHANNELS; ++i) {
      samples[i] = (int16_t)((samples[i] * scale) / 100);
    }
  }

  /*
   * Drop rather than queue without limit. If the device is already further
   * ahead than the target, this frame's audio is late by definition, and
   * queueing it would push every later frame further behind.
   */
  Uint32 queued = SDL_GetQueuedAudioSize(p->audio) /
                  (Uint32)(GBSHARP_AUDIO_CHANNELS * sizeof(int16_t));
  if (queued > (Uint32)(AUDIO_TARGET_FRAMES * 2)) {
    return;
  }

  SDL_QueueAudio(p->audio, samples,
                 (Uint32)(frames * GBSHARP_AUDIO_CHANNELS * sizeof(int16_t)));
}

/* ------------------------------------------------------------------------- */
/* Startup                                                                    */
/* ------------------------------------------------------------------------- */

static uint8_t* read_rom_file(const char* path, size_t* size) {
  SDL_RWops* file = SDL_RWFromFile(path, "rb");
  if (file == NULL) {
    return NULL;
  }

  Sint64 length = SDL_RWsize(file);
  if (length <= 0) {
    SDL_RWclose(file);
    return NULL;
  }

  uint8_t* bytes = (uint8_t*)malloc((size_t)length);
  if (bytes == NULL || SDL_RWread(file, bytes, 1, (size_t)length) != (size_t)length) {
    free(bytes);
    SDL_RWclose(file);
    return NULL;
  }

  SDL_RWclose(file);
  *size = (size_t)length;
  return bytes;
}

int main(int argc, char** argv) {
  player p;
  memset(&p, 0, sizeof(p));
  gbsharp_config_defaults(&p.config);

  if (gbsharp_abi_version() != GBSHARP_ABI_VERSION) {
    fail("GB# Player",
         "This player and the emulator runtime it was built against disagree "
         "about their interface.");
    return 1;
  }

  /*
   * A published game carries its ROM. An unpublished player takes one on the
   * command line, which is what makes it useful as `gbsharp run` before
   * anything has been published.
   */
  const char* rom_argument = NULL;
  const char* screenshot_path = NULL;
  long frame_limit = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      frame_limit = strtol(argv[++i], NULL, 10);
      s_automated = frame_limit > 0;
    } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[++i];
    } else if (rom_argument == NULL) {
      rom_argument = argv[i];
    }
  }

  gbsharp_payload payload;
  const char* payload_error = NULL;
  uint8_t* rom = NULL;
  size_t rom_size = 0;
  bool published = gbsharp_payload_read_self(&payload, &payload_error);

  if (published) {
    rom = payload.rom;
    rom_size = payload.rom_size;
    gbsharp_config_parse(&p.config, payload.config);
  } else if (payload_error != NULL) {
    fail("GB# Player", payload_error);
    return 1;
  } else if (rom_argument != NULL) {
    rom = read_rom_file(rom_argument, &rom_size);
    if (rom == NULL) {
      fail("GB# Player", "That file could not be read.");
      return 1;
    }
  } else {
    fail("GB# Player",
         "This player has no game in it. Publish a game with `gbsharp publish`, "
         "or pass a ROM on the command line.");
    return 1;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fail("GB# Player", SDL_GetError());
    return 1;
  }

  p.emulator = gbsharp_create();
  if (p.emulator == NULL || !gbsharp_load_rom(p.emulator, rom, rom_size)) {
    fail("GB# Player", "This game could not be started: its cartridge data is "
                       "not something the emulator can run.");
    goto shutdown;
  }

  if (published) {
    gbsharp_payload_free(&payload);
  } else {
    free(rom);
  }
  rom = NULL;

  int scale = p.config.scale > 0 ? p.config.scale : 3;
  Uint32 window_flags = SDL_WINDOW_ALLOW_HIGHDPI;
  if (p.config.resizable) {
    window_flags |= SDL_WINDOW_RESIZABLE;
  }
  if (p.config.fullscreen) {
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  }

  p.window = SDL_CreateWindow(p.config.title, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, GB_WIDTH * scale,
                              GB_HEIGHT * scale, window_flags);
  if (p.window == NULL) {
    fail("GB# Player", SDL_GetError());
    goto shutdown;
  }

  /* Vsync to avoid tearing. It does not pace the emulator; the clock below
   * does, because display refresh rates are not 59.7275Hz. */
  p.renderer = SDL_CreateRenderer(p.window, -1, SDL_RENDERER_ACCELERATED |
                                                    SDL_RENDERER_PRESENTVSYNC);
  if (p.renderer == NULL) {
    p.renderer = SDL_CreateRenderer(p.window, -1, 0);
  }
  if (p.renderer == NULL) {
    fail("GB# Player", SDL_GetError());
    goto shutdown;
  }

  /* ABGR8888 in SDL's naming is the same byte order as the ABI's 0xAABBGGRR,
   * so the framebuffer uploads without a conversion pass. */
  p.texture = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_ABGR8888,
                                SDL_TEXTUREACCESS_STREAMING, GB_WIDTH, GB_HEIGHT);
  if (p.texture == NULL) {
    fail("GB# Player", SDL_GetError());
    goto shutdown;
  }
  SDL_SetWindowMinimumSize(p.window, GB_WIDTH, GB_HEIGHT);

  SDL_AudioSpec want;
  SDL_zero(want);
  want.freq = GBSHARP_AUDIO_FREQUENCY;
  want.format = AUDIO_S16SYS;
  want.channels = GBSHARP_AUDIO_CHANNELS;
  want.samples = 1024;
  want.callback = NULL; /* Queued, not called back: the emulator is pulled. */

  SDL_AudioSpec have;
  p.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (p.audio != 0) {
    SDL_PauseAudioDevice(p.audio, 0);
  }
  /* A machine with no sound device still plays the game. */

  p.save_path = save_file_path(p.config.title);
  save_load(&p);

  p.running = true;

  Uint64 frequency = SDL_GetPerformanceFrequency();
  Uint64 previous = SDL_GetPerformanceCounter();
  Uint64 owed_ns = 0;
  Uint64 since_save_ns = 0;

  while (p.running) {
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 elapsed_ns = (now - previous) * 1000000000ull / frequency;
    previous = now;

    owed_ns += elapsed_ns;
    since_save_ns += elapsed_ns;

    if (owed_ns > FRAME_PERIOD_NS * MAX_CATCHUP_FRAMES) {
      owed_ns = FRAME_PERIOD_NS * MAX_CATCHUP_FRAMES;
    }

    handle_events(&p);

    bool drew = false;
    while (owed_ns >= FRAME_PERIOD_NS && p.running) {
      owed_ns -= FRAME_PERIOD_NS;
      gbsharp_run_frame(p.emulator);
      pump_audio(&p);
      drew = true;

      if (frame_limit > 0 && --frame_limit == 0) {
        p.running = false;
      }
    }

    if (drew) {
      present(&p);
    } else {
      /* Nothing due yet. Yielding keeps a published game off a whole core of
       * somebody's laptop. */
      SDL_Delay(1);
    }

    if (since_save_ns >= 1000000000ull) {
      since_save_ns = 0;
      save_store(&p, false);
    }
  }

  save_store(&p, true);

  /*
   * Writes what the screen was showing when the run ended. Used by CI to check
   * that a published game reaches its title screen rather than merely failing
   * to crash, which is all an exit code can tell you about something whose
   * whole job is to put a picture on a display.
   */
  if (screenshot_path != NULL) {
    const uint32_t* pixels = gbsharp_get_framebuffer(p.emulator);
    SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormatFrom(
        (void*)pixels, GB_WIDTH, GB_HEIGHT, 32,
        GB_WIDTH * (int)sizeof(uint32_t), SDL_PIXELFORMAT_ABGR8888);
    if (shot != NULL) {
      SDL_SaveBMP(shot, screenshot_path);
      SDL_FreeSurface(shot);
    }
  }

shutdown:
  free(rom);
  if (p.save_path != NULL) {
    free(p.save_path);
  }
  if (p.controller != NULL) {
    SDL_GameControllerClose(p.controller);
  }
  if (p.audio != 0) {
    SDL_CloseAudioDevice(p.audio);
  }
  if (p.texture != NULL) {
    SDL_DestroyTexture(p.texture);
  }
  if (p.renderer != NULL) {
    SDL_DestroyRenderer(p.renderer);
  }
  if (p.window != NULL) {
    SDL_DestroyWindow(p.window);
  }
  if (p.emulator != NULL) {
    gbsharp_destroy(p.emulator);
  }
  SDL_Quit();
  return 0;
}
