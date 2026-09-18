#pragma once

#include <Arduino.h>

#ifndef LOG_LEVEL
#define LOG_LEVEL 3
#endif

#define LOG_AT(lvl, letter, tag, fmt, ...)                                      \
  do {                                                                          \
    if (LOG_LEVEL >= (lvl)) {                                                   \
      Serial.printf("[%8lu] " letter " %s: " fmt "\n", (unsigned long)millis(), \
                    tag, ##__VA_ARGS__);                                        \
    }                                                                           \
  } while (0)

#define LOG_E(tag, fmt, ...) LOG_AT(1, "E", tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) LOG_AT(2, "W", tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) LOG_AT(3, "I", tag, fmt, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) LOG_AT(4, "D", tag, fmt, ##__VA_ARGS__)
