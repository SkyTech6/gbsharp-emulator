/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>

void gbsharp_config_defaults(gbsharp_config* config) {
  memset(config, 0, sizeof(*config));
  /* Named for the runtime rather than for a game, so that an unpublished
   * player says what it is. A published one always overrides this. */
  strcpy(config->title, "GB# Player");
  config->scale = 3;
  config->fullscreen = false;
  config->resizable = true;
  config->integer_scaling = true;
  config->volume = 100;
}

static const char* skip_space(const char* s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
    ++s;
  }
  return s;
}

/*
 * Reads a JSON string into `out`, returning where it ended, or NULL if the
 * input is not a string. Handles the escapes a window title can plausibly
 * contain and passes anything else through as written, which is the honest
 * behaviour for a parser this size.
 */
static const char* parse_string(const char* s, char* out, size_t capacity) {
  if (*s != '"') {
    return NULL;
  }
  ++s;

  size_t length = 0;
  while (*s != '\0' && *s != '"') {
    char c = *s++;
    if (c == '\\' && *s != '\0') {
      char escape = *s++;
      switch (escape) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'u':
          /* A code point needs UTF-8 encoding and a surrogate pair dance to do
           * properly. Skipping the four digits keeps the rest of the title
           * intact, which beats truncating it. */
          for (int i = 0; i < 4 && *s != '\0'; ++i) {
            ++s;
          }
          continue;
        default: c = escape; break;
      }
    }

    if (out != NULL && length + 1 < capacity) {
      out[length++] = c;
    }
  }

  if (*s != '"') {
    return NULL;
  }

  if (out != NULL && capacity > 0) {
    out[length] = '\0';
  }
  return s + 1;
}

/* Skips one value of any type, so an unknown key cannot derail the scan. */
static const char* skip_value(const char* s) {
  s = skip_space(s);

  if (*s == '"') {
    return parse_string(s, NULL, 0);
  }

  if (*s == '{' || *s == '[') {
    char open = *s;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (*s != '\0') {
      if (*s == '"') {
        const char* end = parse_string(s, NULL, 0);
        if (end == NULL) {
          return NULL;
        }
        s = end;
        continue;
      }
      if (*s == open) {
        ++depth;
      } else if (*s == close) {
        if (--depth == 0) {
          return s + 1;
        }
      }
      ++s;
    }
    return NULL;
  }

  while (*s != '\0' && *s != ',' && *s != '}') {
    ++s;
  }
  return s;
}

static bool key_is(const char* key, const char* name) {
  return strcmp(key, name) == 0;
}

void gbsharp_config_parse(gbsharp_config* config, const char* json) {
  if (json == NULL) {
    return;
  }

  const char* s = skip_space(json);
  if (*s != '{') {
    return;
  }
  ++s;

  for (;;) {
    s = skip_space(s);
    if (*s == '}' || *s == '\0') {
      return;
    }

    char key[64];
    const char* after_key = parse_string(s, key, sizeof(key));
    if (after_key == NULL) {
      return;
    }

    s = skip_space(after_key);
    if (*s != ':') {
      return;
    }
    s = skip_space(s + 1);

    if (key_is(key, "title")) {
      const char* end = parse_string(s, config->title, sizeof(config->title));
      s = (end != NULL) ? end : skip_value(s);
    } else if (key_is(key, "scale")) {
      config->scale = atoi(s);
      s = skip_value(s);
    } else if (key_is(key, "volume")) {
      config->volume = atoi(s);
      s = skip_value(s);
    } else if (key_is(key, "fullscreen")) {
      config->fullscreen = (*s == 't');
      s = skip_value(s);
    } else if (key_is(key, "resizable")) {
      config->resizable = (*s == 't');
      s = skip_value(s);
    } else if (key_is(key, "integerScaling")) {
      config->integer_scaling = (*s == 't');
      s = skip_value(s);
    } else {
      s = skip_value(s);
    }

    if (s == NULL) {
      return;
    }

    s = skip_space(s);
    if (*s == ',') {
      ++s;
    }
  }
}
