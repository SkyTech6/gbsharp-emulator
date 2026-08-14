/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include "payload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

/* Little endian readers, so a payload written on one machine is read the same
 * on another. Every platform GB# targets is little endian, but a published game
 * is a file that travels and this costs nothing. */
static uint32_t read_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t* p) {
  return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static uint32_t checksum(const uint8_t* data, size_t size) {
  /* Adler-32: enough to catch truncation and corruption, and short enough to
   * be obviously correct. This is not a security boundary; a payload that has
   * been tampered with deliberately is a signing question, not a checksum one. */
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

char* gbsharp_executable_path(void) {
#if defined(_WIN32)
  DWORD capacity = MAX_PATH;
  for (;;) {
    char* buffer = (char*)malloc(capacity);
    if (buffer == NULL) {
      return NULL;
    }
    DWORD length = GetModuleFileNameA(NULL, buffer, capacity);
    if (length == 0) {
      free(buffer);
      return NULL;
    }
    /* Truncation is reported by filling the buffer exactly, so grow and retry
     * rather than trusting a path that may have lost its tail. */
    if (length < capacity) {
      return buffer;
    }
    free(buffer);
    if (capacity >= 65536) {
      return NULL;
    }
    capacity *= 2;
  }
#elif defined(__APPLE__)
  uint32_t capacity = 1024;
  char* buffer = (char*)malloc(capacity);
  if (buffer == NULL) {
    return NULL;
  }
  if (_NSGetExecutablePath(buffer, &capacity) != 0) {
    /* capacity now holds what is needed. */
    char* grown = (char*)realloc(buffer, capacity);
    if (grown == NULL) {
      free(buffer);
      return NULL;
    }
    buffer = grown;
    if (_NSGetExecutablePath(buffer, &capacity) != 0) {
      free(buffer);
      return NULL;
    }
  }
  return buffer;
#else
  size_t capacity = 1024;
  for (;;) {
    char* buffer = (char*)malloc(capacity);
    if (buffer == NULL) {
      return NULL;
    }
    ssize_t length = readlink("/proc/self/exe", buffer, capacity - 1);
    if (length < 0) {
      free(buffer);
      return NULL;
    }
    if ((size_t)length < capacity - 1) {
      buffer[length] = '\0';
      return buffer;
    }
    free(buffer);
    if (capacity >= 65536) {
      return NULL;
    }
    capacity *= 2;
  }
#endif
}

bool gbsharp_payload_read_file(const char* path, gbsharp_payload* payload,
                               const char** error) {
  if (error != NULL) {
    *error = NULL;
  }
  memset(payload, 0, sizeof(*payload));

  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    if (error != NULL) {
      *error = "the executable could not be opened to read its payload";
    }
    return false;
  }

  uint8_t trailer_bytes[GBSHARP_PAYLOAD_TRAILER_SIZE];
  long long file_size;

  if (fseek(file, 0, SEEK_END) != 0) {
    goto not_published;
  }
  file_size = ftell(file);
  if (file_size < (long long)sizeof(trailer_bytes)) {
    goto not_published;
  }
  if (fseek(file, -(long)sizeof(trailer_bytes), SEEK_END) != 0) {
    goto not_published;
  }
  if (fread(trailer_bytes, sizeof(trailer_bytes), 1, file) != 1) {
    goto not_published;
  }

  gbsharp_payload_trailer trailer;
  trailer.magic = read_u32(trailer_bytes + 0);
  trailer.version = read_u32(trailer_bytes + 4);
  trailer.rom_offset = read_u64(trailer_bytes + 8);
  trailer.rom_size = read_u64(trailer_bytes + 16);
  trailer.config_offset = read_u64(trailer_bytes + 24);
  trailer.config_size = read_u64(trailer_bytes + 32);
  trailer.checksum = read_u32(trailer_bytes + 40);
  trailer.magic_end = read_u32(trailer_bytes + 44);

  /* No magic means an unpublished player, which is not an error. */
  if (trailer.magic != GBSHARP_PAYLOAD_MAGIC ||
      trailer.magic_end != GBSHARP_PAYLOAD_MAGIC) {
    goto not_published;
  }

  /* Past this point a payload exists, so every failure is worth a message:
   * silently falling back to "no game" would be the confusing outcome. */
  if (trailer.version != GBSHARP_PAYLOAD_VERSION) {
    if (error != NULL) {
      *error = "this game was published by a newer version of GB# than this "
               "player understands";
    }
    goto failed;
  }

  if (trailer.rom_size == 0 ||
      trailer.rom_offset + trailer.rom_size > (uint64_t)file_size ||
      trailer.config_offset + trailer.config_size > (uint64_t)file_size) {
    if (error != NULL) {
      *error = "the game data in this file is truncated or its offsets are "
               "out of range";
    }
    goto failed;
  }

  payload->rom = (uint8_t*)malloc((size_t)trailer.rom_size);
  payload->config = (char*)malloc((size_t)trailer.config_size + 1);
  if (payload->rom == NULL || payload->config == NULL) {
    if (error != NULL) {
      *error = "there was not enough memory to read the game data";
    }
    goto failed;
  }

  if (fseek(file, (long)trailer.rom_offset, SEEK_SET) != 0 ||
      fread(payload->rom, (size_t)trailer.rom_size, 1, file) != 1) {
    if (error != NULL) {
      *error = "the game data in this file could not be read";
    }
    goto failed;
  }

  if (trailer.config_size > 0) {
    if (fseek(file, (long)trailer.config_offset, SEEK_SET) != 0 ||
        fread(payload->config, (size_t)trailer.config_size, 1, file) != 1) {
      if (error != NULL) {
        *error = "the settings in this file could not be read";
      }
      goto failed;
    }
  }
  payload->config[trailer.config_size] = '\0';

  payload->rom_size = (size_t)trailer.rom_size;
  payload->config_size = (size_t)trailer.config_size;

  {
    uint32_t actual = checksum(payload->rom, payload->rom_size);
    actual += checksum((const uint8_t*)payload->config, payload->config_size);
    if (actual != trailer.checksum) {
      if (error != NULL) {
        *error = "this file is damaged: the game data does not match its "
                 "checksum";
      }
      goto failed;
    }
  }

  fclose(file);
  return true;

not_published:
  fclose(file);
  gbsharp_payload_free(payload);
  return false;

failed:
  fclose(file);
  gbsharp_payload_free(payload);
  return false;
}

bool gbsharp_payload_read_self(gbsharp_payload* payload, const char** error) {
  char* path = gbsharp_executable_path();
  if (path == NULL) {
    memset(payload, 0, sizeof(*payload));
    if (error != NULL) {
      *error = "this program could not find its own location on disk";
    }
    return false;
  }

  bool result = gbsharp_payload_read_file(path, payload, error);
  free(path);
  return result;
}

void gbsharp_payload_free(gbsharp_payload* payload) {
  free(payload->rom);
  free(payload->config);
  memset(payload, 0, sizeof(*payload));
}
