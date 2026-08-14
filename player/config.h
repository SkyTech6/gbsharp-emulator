/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * How a published game wants its window to behave.
 *
 * Every one of these comes from the game's gbsharp.json, is written into the
 * published executable by `gbsharp publish`, and is read back here at startup.
 * The player has no settings UI and no configuration file of its own, on
 * purpose: a game decides how it presents itself, and a player that could
 * disagree with the game would be an emulator wearing the game's name.
 */
#ifndef GBSHARP_PLAYER_CONFIG_H_
#define GBSHARP_PLAYER_CONFIG_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBSHARP_CONFIG_TITLE_MAX 128

typedef struct gbsharp_config {
  char title[GBSHARP_CONFIG_TITLE_MAX];

  /* Window size as a multiple of 160x144. Clamped to something that fits. */
  int scale;

  bool fullscreen;
  bool resizable;

  /* Scale only by whole numbers, so pixels stay square and evenly sized. Off
   * means fill the window, preserving aspect ratio. */
  bool integer_scaling;

  /* 0 to 100. */
  int volume;
} gbsharp_config;

/* The defaults a game gets by saying nothing. */
void gbsharp_config_defaults(gbsharp_config* config);

/*
 * Fills in whatever the JSON names, leaving the rest at its default.
 *
 * The parser handles one flat object of strings, numbers and booleans, which is
 * the entire shape this file has ever needed. It is not a general JSON parser
 * and does not pretend to be: unknown keys are skipped, and malformed input
 * loses the settings rather than the game, because a game that will not start
 * because of a typo in a window title is the worse failure.
 */
void gbsharp_config_parse(gbsharp_config* config, const char* json);

#ifdef __cplusplus
}
#endif

#endif /* GBSHARP_PLAYER_CONFIG_H_ */
