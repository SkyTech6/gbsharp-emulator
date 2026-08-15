/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * The payload a published game carries inside its own executable.
 *
 * `gbsharp publish` produces a game by copying the prebuilt player and
 * appending to it:
 *
 *     MyGame.exe = gbsharp-player.exe
 *                + [ROM bytes]
 *                + [config JSON]
 *                + [trailer]
 *
 * The player opens its own executable at startup, reads the trailer from the
 * last bytes of the file, and loads the ROM from memory. Nothing is unpacked to
 * disk and nothing is relinked, which is what lets publishing work on a machine
 * with no C toolchain, and lets it work identically on every platform.
 *
 * Appending to an executable is safe because every format we target describes
 * its own extent in its header: PE, ELF and Mach-O loaders all ignore trailing
 * bytes. The costs are real and known. Code signing has to happen after the
 * append, so it belongs to `gbsharp publish` rather than to the runtime's
 * release CI. Icons and version metadata are baked into the prebuilt stub.
 */
#ifndef GBSHARP_PLAYER_PAYLOAD_H_
#define GBSHARP_PLAYER_PAYLOAD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "GB#P" little endian, at both ends of the trailer. */
#define GBSHARP_PAYLOAD_MAGIC 0x50234247u

/* Bumped if the trailer layout changes. A player refuses a payload it does not
 * understand rather than reading offsets out of a format it is guessing at. */
#define GBSHARP_PAYLOAD_VERSION 1

/*
 * Written little endian at the very end of the file, so it is found by seeking
 * backwards from the end rather than by scanning. Fixed width fields only, and
 * no padding worth arguing about: 64 bit offsets because a stub plus a ROM can
 * exceed 4GB in principle, even though a Game Boy ROM cannot.
 */
typedef struct gbsharp_payload_trailer {
  uint32_t magic;
  uint32_t version;
  uint64_t rom_offset;
  uint64_t rom_size;
  uint64_t config_offset;
  uint64_t config_size;
  /* Sum of the payload bytes, so a truncated download is caught before the
   * emulator is asked to make sense of half a cartridge. */
  uint32_t checksum;
  uint32_t magic_end;
} gbsharp_payload_trailer;

#define GBSHARP_PAYLOAD_TRAILER_SIZE 48

typedef struct gbsharp_payload {
  uint8_t* rom;
  size_t rom_size;
  /* NUL terminated for the JSON parser's convenience. May be empty. */
  char* config;
  size_t config_size;
} gbsharp_payload;

/*
 * Reads the payload appended to this process's own executable.
 *
 * Returns false when there is no payload, which is the normal state of the
 * unpublished player and not an error: the caller falls back to a ROM named on
 * the command line. `error` receives a sentence describing a payload that is
 * present but unusable, and is set to NULL when there simply is not one.
 *
 * On success the caller owns `payload->rom` and `payload->config`, both of
 * which are freed by gbsharp_payload_free.
 */
bool gbsharp_payload_read_self(gbsharp_payload* payload, const char** error);

/* Reads from a named file instead, which is how the tests check the format. */
bool gbsharp_payload_read_file(const char* path, gbsharp_payload* payload,
                               const char** error);

void gbsharp_payload_free(gbsharp_payload* payload);

/* The path of the running executable, or NULL. Caller frees. */
char* gbsharp_executable_path(void);

#ifdef __cplusplus
}
#endif

#endif /* GBSHARP_PLAYER_PAYLOAD_H_ */
