#pragma once

// Shared console format for the hardware verification apps. The serial output is
// the whole diagnostic: a failing step must explain itself well enough that the
// operator never has to open the firmware to interpret it.

#include <Arduino.h>
#include <string.h>

class CheckReport {
 public:
  void begin(const char* title) {
    Serial.println();
    rule();
    Serial.printf("  %s\n", title);
    rule();
  }

  void step(const char* name) {
    finishOpen();
    int written = Serial.printf("  %d. %s ", ++index_, name);
    for (int i = written; i < kVerdictColumn; ++i) {
      Serial.print('.');
    }
    Serial.print(' ');
    open_ = true;
  }

  void pass(const char* detail = nullptr) {
    verdict("PASS", detail);
    ++passed_;
  }

  void fail(const char* why) {
    verdict("FAIL", why);
    ++failed_;
  }

  void warn(const char* why) {
    verdict("WARN", why);
    ++warned_;
  }

  // Steps that observe rather than prove. Counted separately so they can never
  // turn the run green or red on their own.
  void observed(const char* detail) { verdict("INFO", detail); }

  void hint(const char* text) {
    finishOpen();
    Serial.printf("        -> %s\n", text);
  }

  void note(const char* text) {
    finishOpen();
    Serial.printf("  %s\n", text);
  }

  void blank() {
    finishOpen();
    Serial.println();
  }

  bool summary() {
    finishOpen();
    rule();
    Serial.printf("  %d passed, %d failed, %d warnings\n", passed_, failed_, warned_);
    Serial.printf("  %s\n", failed_ == 0 ? "RESULT: OK" : "RESULT: WIRING FAULT");
    rule();
    Serial.println();
    return failed_ == 0;
  }

 private:
  static const int kVerdictColumn = 54;

  void rule() {
    for (int i = 0; i < 64; ++i) {
      Serial.print('=');
    }
    Serial.println();
  }

  void verdict(const char* tag, const char* detail) {
    Serial.print(tag);
    if (detail != nullptr) {
      Serial.printf("  %s", detail);
    }
    Serial.println();
    open_ = false;
  }

  void finishOpen() {
    if (open_) {
      Serial.println();
      open_ = false;
    }
  }

  int index_ = 0;
  int passed_ = 0;
  int failed_ = 0;
  int warned_ = 0;
  bool open_ = false;
};

static CheckReport report;
