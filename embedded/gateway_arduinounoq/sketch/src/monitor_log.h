#pragma once

#include <stdarg.h>
#include <stdio.h>

#include "Arduino_RouterBridge.h"

namespace gw {

// Every line goes to the MPU as well as to Monitor. arduino-app-cli monitor
// attaches nothing while the app is running, so Monitor alone would hide exactly
// the messages worth having - a radio that would not configure, a reply that
// never went out - while the MPU log is followable at any time.
//
// 128 bytes keeps one line well inside RPClite's 256-byte request buffer.
inline void logf(const char* fmt, ...) {
  char line[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Monitor.println(line);
  Bridge.notify("on_mcu_log", line);
}

}  // namespace gw
