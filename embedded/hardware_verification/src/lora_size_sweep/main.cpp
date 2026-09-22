// Independent measurement of the E32-900T20D air-package limit in fixed
// transmission mode, sweeping 50..65 byte payloads across a real link.
//
// Written to settle one disagreement with the manual: whether a write longer
// than the limit is sub-packed, as section 2.2 implies, or truncated. A byte
// count alone cannot tell truncation from corruption or reordering, so every
// payload here carries its own declared length, an ascending filler pattern and
// a checksum. The receiver can then say whether what arrived is a clean prefix
// of what was sent, which is the only evidence that settles the question.
//
// Deliberately does not use silometer_endpoint/lib/e32 - the point is a second
// opinion, so the UART, mode and AUX handling below are independent of it.
//
// Build -DROLE_TX on one board and -DROLE_RX on the other.

#include <Arduino.h>
#include <string.h>

#include "board_pins.h"

#if !defined(ROLE_TX) && !defined(ROLE_RX)
#error "build with -DROLE_TX or -DROLE_RX"
#endif

namespace {

const uint8_t MAGIC = 0x5A;
const uint8_t TYPE_HELLO = 0x01;
const uint8_t TYPE_READY = 0x02;
const uint8_t TYPE_SAMPLE = 0x03;

const uint16_t ADDR_TX = 0x0007;
const uint16_t ADDR_RX = 0x0001;
const uint8_t CHAN_915 = 0x35;
const uint8_t SPED_9600_8N1_2K4 = 0x1A;
const uint8_t OPTION_FIXED_20DBM = 0xC4;

const uint8_t ROUTING_BYTES = 3;
const uint8_t HANDSHAKE_LEN = 8;
const uint8_t SWEEP_MIN = 50;
const uint8_t SWEEP_MAX = 65;

// Manual section 5.6.4 note 3: a mode switch only takes effect once AUX has
// been high for 2 ms. The margin over that costs nothing on a bench app.
const uint32_t AUX_STABLE_MS = 5;
const uint32_t AUX_TIMEOUT_MS = 2000;
// Sections 5.5, 5.6.4 note 4 and 7.4: power-on self-check, leaving mode 3 and a
// C4 reset all hold AUX low far longer than an ordinary switch.
const uint32_t AUX_RESET_TIMEOUT_MS = 3000;

// Section 6.2: AUX drops when the module takes the first byte of a packet and
// rises once the packet has reached the RF chip. Whether that dip appears is
// what distinguishes a module that accepted the whole write from one that
// ignored the tail, so it is polled finely and reported per sample.
const uint32_t AUX_DIP_TIMEOUT_MS = 300;

// A byte is ~1 ms at 9600 baud, so a gap this long can only be a package
// boundary, and this much silence can only be the end of a burst.
const uint32_t PACKAGE_GAP_MS = 20;
const uint32_t BURST_QUIET_MS = 500;

const uint32_t SAMPLE_SPACING_MS = 2500;
const uint16_t RX_CAPACITY = 256;

void setModePins(uint8_t m0, uint8_t m1) {
  digitalWrite(PIN_LORA_M0, m0);
  digitalWrite(PIN_LORA_M1, m1);
}

bool waitAuxHigh(uint32_t stableMs, uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint32_t highSince = 0;
  while (millis() - start < timeoutMs) {
    if (digitalRead(PIN_LORA_AUX) == HIGH) {
      if (highSince == 0) {
        highSince = millis();
      }
      if (millis() - highSince >= stableMs) {
        return true;
      }
    } else {
      highSince = 0;
    }
    delay(1);
  }
  return false;
}

// Polled far finer than the millisecond waits: the dip after a write can be
// short, and missing it would be reported as the module refusing the data.
bool waitAuxLow(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (digitalRead(PIN_LORA_AUX) == LOW) {
      return true;
    }
    delayMicroseconds(100);
  }
  return false;
}

void drainUart() {
  while (Serial2.available()) {
    Serial2.read();
  }
}

uint8_t readAnswer(uint8_t* buf, uint8_t expected, uint32_t timeoutMs) {
  uint8_t got = 0;
  const uint32_t start = millis();
  while (got < expected && millis() - start < timeoutMs) {
    if (Serial2.available()) {
      buf[got++] = (uint8_t)Serial2.read();
    }
  }
  return got;
}

bool enterMode(uint8_t m0, uint8_t m1, bool leavingSleep) {
  if (!waitAuxHigh(AUX_STABLE_MS, AUX_TIMEOUT_MS)) {
    return false;
  }
  setModePins(m0, m1);
  delay(10);
  if (!waitAuxHigh(AUX_STABLE_MS, leavingSleep ? AUX_RESET_TIMEOUT_MS : AUX_TIMEOUT_MS)) {
    return false;
  }
  drainUart();
  return true;
}

bool sendCommand(const uint8_t* cmd, uint8_t len) {
  if (!waitAuxHigh(AUX_STABLE_MS, AUX_TIMEOUT_MS)) {
    return false;
  }
  drainUart();
  Serial2.write(cmd, len);
  Serial2.flush();
  return true;
}

bool readParams(uint8_t* out) {
  const uint8_t cmd[3] = {0xC1, 0xC1, 0xC1};
  if (!sendCommand(cmd, sizeof(cmd))) {
    return false;
  }
  return readAnswer(out, 6, 1000) == 6 && (out[0] == 0xC0 || out[0] == 0xC2);
}

bool configure(uint16_t address) {
  uint8_t current[6] = {0};
  if (!readParams(current)) {
    Serial.println("  parameter read failed");
    return false;
  }
  const uint8_t wanted[6] = {0xC0,
                             (uint8_t)(address >> 8),
                             (uint8_t)(address & 0xFF),
                             SPED_9600_8N1_2K4,
                             CHAN_915,
                             OPTION_FIXED_20DBM};
  if (memcmp(current, wanted, sizeof(wanted)) == 0) {
    Serial.printf("  already set: addr 0x%04X chan 0x%02X option 0x%02X\n", address, CHAN_915,
                  OPTION_FIXED_20DBM);
    return true;
  }

  Serial.printf("  rewriting %02X %02X %02X %02X %02X %02X -> %02X %02X %02X %02X %02X %02X\n",
                current[0], current[1], current[2], current[3], current[4], current[5], wanted[0],
                wanted[1], wanted[2], wanted[3], wanted[4], wanted[5]);
  if (!sendCommand(wanted, sizeof(wanted))) {
    return false;
  }
  uint8_t echo[6] = {0};
  readAnswer(echo, 6, 1000);

  // Section 7.4: verifying through a reset is what makes this a persistence
  // check rather than a live-register one.
  const uint8_t reset[3] = {0xC4, 0xC4, 0xC4};
  if (!sendCommand(reset, sizeof(reset))) {
    return false;
  }
  delay(10);
  if (!waitAuxHigh(AUX_STABLE_MS, AUX_RESET_TIMEOUT_MS)) {
    Serial.println("  module never returned from the C4 reset");
    return false;
  }
  drainUart();

  uint8_t readback[6] = {0};
  if (!readParams(readback) || memcmp(readback, wanted, sizeof(wanted)) != 0) {
    Serial.println("  parameters did not persist");
    return false;
  }
  Serial.println("  parameters written and verified through a reset");
  return true;
}

uint8_t buildFrame(uint8_t* buf, uint8_t type, uint8_t total, uint8_t seq) {
  buf[0] = MAGIC;
  buf[1] = type;
  buf[2] = total;
  buf[3] = seq;
  for (uint8_t i = 4; i + 1 < total; ++i) {
    buf[i] = i;
  }
  uint8_t sum = 0;
  for (uint8_t i = 0; i + 1 < total; ++i) {
    sum ^= buf[i];
  }
  buf[total - 1] = sum;
  return total;
}

void dumpHex(const uint8_t* buf, uint16_t len) {
  for (uint16_t i = 0; i < len; ++i) {
    if (i % 16 == 0) {
      Serial.printf("\n      %3u: ", i);
    }
    Serial.printf("%02X ", buf[i]);
  }
  Serial.println();
}

bool sendFixed(uint16_t dst, const uint8_t* payload, uint8_t len, bool* sawDip) {
  *sawDip = false;
  if (!waitAuxHigh(AUX_STABLE_MS, AUX_TIMEOUT_MS)) {
    return false;
  }
  uint8_t out[ROUTING_BYTES + 255];
  out[0] = (uint8_t)(dst >> 8);
  out[1] = (uint8_t)(dst & 0xFF);
  out[2] = CHAN_915;
  memcpy(out + ROUTING_BYTES, payload, len);
  Serial2.write(out, (size_t)(ROUTING_BYTES + len));
  Serial2.flush();

  *sawDip = waitAuxLow(AUX_DIP_TIMEOUT_MS);
  return waitAuxHigh(AUX_STABLE_MS, AUX_TIMEOUT_MS);
}

uint16_t awaitBurst(uint8_t* buf, uint16_t cap, uint8_t* packages, uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint16_t total = 0;
  uint32_t lastByte = 0;
  *packages = 0;
  while (true) {
    if (Serial2.available()) {
      const uint32_t now = millis();
      const uint8_t b = (uint8_t)Serial2.read();
      if (total == 0 || (now - lastByte) > PACKAGE_GAP_MS) {
        ++(*packages);
      }
      if (total < cap) {
        buf[total] = b;
      }
      ++total;
      lastByte = now;
      continue;
    }
    if (total > 0 && millis() - lastByte > BURST_QUIET_MS) {
      return total < cap ? total : cap;
    }
    if (total == 0 && millis() - start > timeoutMs) {
      return 0;
    }
    delay(1);
  }
}

}  // namespace

#if defined(ROLE_RX)
namespace {

void reportFrame(const uint8_t* buf, uint16_t total, uint8_t packages) {
  Serial.printf("  %u byte(s) in %u air package(s)\n", total, packages);
  if (total < 4 || buf[0] != MAGIC) {
    Serial.println("  not one of ours");
    dumpHex(buf, total);
    return;
  }

  const uint8_t declared = buf[2];
  const char* verdict = total == declared ? "COMPLETE" : (total < declared ? "TRUNCATED" : "OVERLONG");
  Serial.printf("  type=0x%02X seq=%u declared=%u actual=%u -> %s\n", buf[1], buf[3], declared,
                total, verdict);

  const uint16_t patternEnd = (total < declared) ? total : (uint16_t)(declared - 1);
  uint16_t mismatches = 0;
  for (uint16_t i = 4; i < patternEnd; ++i) {
    if (buf[i] != (uint8_t)i) {
      ++mismatches;
    }
  }
  Serial.printf("  filler pattern over bytes 4..%u: %u mismatch(es)%s\n",
                patternEnd > 4 ? patternEnd - 1 : 4, mismatches,
                mismatches ? "" : "  (a clean prefix of what was sent)");

  if (total == declared) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i + 1 < declared; ++i) {
      sum ^= buf[i];
    }
    Serial.printf("  checksum: %s\n", sum == buf[declared - 1] ? "ok" : "BAD");
  }
  dumpHex(buf, total);
}

void runRx() {
  Serial.println("\nreceiver ready, waiting for HELLO then the sweep\n");
  uint8_t buf[RX_CAPACITY];
  while (true) {
    uint8_t packages = 0;
    const uint16_t got = awaitBurst(buf, sizeof(buf), &packages, 30000);
    if (got == 0) {
      Serial.println("(nothing for 30 s)");
      continue;
    }
    Serial.println();
    reportFrame(buf, got, packages);

    if (got >= 4 && buf[0] == MAGIC && buf[1] == TYPE_HELLO) {
      uint8_t reply[HANDSHAKE_LEN];
      buildFrame(reply, TYPE_READY, HANDSHAKE_LEN, buf[3]);
      delay(50);  // let the far end finish its turnaround before answering
      bool dip = false;
      Serial.printf("  -> READY %s\n", sendFixed(ADDR_TX, reply, HANDSHAKE_LEN, &dip) ? "sent" : "FAILED");
    }
  }
}

}  // namespace
#endif

#if defined(ROLE_TX)
namespace {

bool handshake() {
  uint8_t frame[HANDSHAKE_LEN];
  uint8_t reply[64];
  for (uint8_t attempt = 1; attempt <= 10; ++attempt) {
    buildFrame(frame, TYPE_HELLO, HANDSHAKE_LEN, attempt);
    bool dip = false;
    Serial.printf("HELLO attempt %u ... ", attempt);
    if (!sendFixed(ADDR_RX, frame, HANDSHAKE_LEN, &dip)) {
      Serial.println("send failed");
      continue;
    }
    uint8_t packages = 0;
    const uint16_t got = awaitBurst(reply, sizeof(reply), &packages, 2000);
    if (got >= 4 && reply[0] == MAGIC && reply[1] == TYPE_READY) {
      Serial.println("READY received");
      return true;
    }
    Serial.printf("no READY (%u bytes back)\n", got);
    delay(500);
  }
  return false;
}

void runTx() {
  // The sweep is worthless if the receiver was never listening, so a confirmed
  // round trip is a precondition rather than a diagnostic.
  if (!handshake()) {
    Serial.println("\nhandshake failed - receiver never answered. Sweep NOT run.");
    return;
  }

  Serial.printf("\nsweeping %u..%u byte payloads, %u routing bytes on top\n", SWEEP_MIN, SWEEP_MAX,
                ROUTING_BYTES);
  uint8_t frame[256];
  for (uint8_t len = SWEEP_MIN; len <= SWEEP_MAX; ++len) {
    buildFrame(frame, TYPE_SAMPLE, len, len);
    bool dip = false;
    const bool ok = sendFixed(ADDR_RX, frame, len, &dip);
    Serial.printf("\nsent declared=%u  uart write=%u  AUX dip %s%s", len, len + ROUTING_BYTES,
                  dip ? "seen" : "NOT SEEN",
                  ok ? "" : "  (AUX never came back high)");
    dumpHex(frame, len);
    delay(SAMPLE_SPACING_MS);
  }
  Serial.println("\nsweep done");
}

}  // namespace
#endif

void setup() {
  Serial.begin(CONSOLE_BAUD);

  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  pinMode(PIN_LORA_AUX, INPUT_PULLUP);
  // Driven before the first AUX sample so the module finishes its power-on
  // self-check already in config mode (manual section 5.5).
  setModePins(HIGH, HIGH);

  delay(300);
#if defined(ROLE_TX)
  Serial.println("\n\n=== E32 AIR-PACKAGE SIZE SWEEP - TRANSMITTER ===");
  const uint16_t ownAddress = ADDR_TX;
#else
  Serial.println("\n\n=== E32 AIR-PACKAGE SIZE SWEEP - RECEIVER ===");
  const uint16_t ownAddress = ADDR_RX;
#endif

  if (!waitAuxHigh(AUX_STABLE_MS, AUX_RESET_TIMEOUT_MS)) {
    Serial.println("ABORT: module never finished its power-on self-check");
    return;
  }
  Serial2.begin(BAUD_RATE_UART_LORA_E32900T20D_DEFAULT, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);

  Serial.println("configuring:");
  if (!configure(ownAddress)) {
    Serial.println("ABORT: could not configure the module");
    return;
  }
  if (!enterMode(LOW, LOW, true)) {
    Serial.println("ABORT: module never settled into mode 0");
    return;
  }

#if defined(ROLE_TX)
  runTx();
#else
  runRx();
#endif
}

void loop() {}
