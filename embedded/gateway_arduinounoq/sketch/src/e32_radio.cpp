#include "e32_radio.h"

#include <Arduino.h>
#include <string.h>

#include "gateway_config.h"
#include "monitor_log.h"

namespace gw {
namespace {

constexpr uint8_t kCmdReadConfig = 0xC1;
constexpr uint8_t kCmdWriteSaved = 0xC0;
constexpr uint8_t kCmdWriteLive = 0xC2;
constexpr uint8_t kCmdReset = 0xC4;
constexpr uint8_t kConfigBytes = 6;

}  // namespace

// AUX low means busy. Readiness is a sustained high rather than an edge -
// sampling a moment late would block until the timeout on a command
// that actually succeeded, turning a working write into a spurious failure.
bool E32Radio::waitAuxStable(uint32_t stableMs, uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint32_t highSince = 0;
  while (millis() - start < timeoutMs) {
    if (digitalRead(kLoraAuxPin) == HIGH) {
      if (highSince == 0) {
        highSince = millis();
      }
      if (millis() - highSince >= stableMs) {
        return true;
      }
    } else {
      highSince = 0;
    }
    delay(1);  // never busy-spin: this thread also has to let the Bridge run
  }
  return false;
}

// Section 5.6.4: AUX low means busy, and a module that is still answering the
// previous command will not take the next one. Back-to-back commands are the
// normal case here - the C0 write follows the C1 read by microseconds - so the
// gate belongs on every command rather than only on mode switches.
bool E32Radio::sendCommand(const uint8_t* cmd, uint8_t len) {
  if (!waitAuxStable(cfg::kAuxStableMs, cfg::kAuxTimeoutMs)) {
    logf("e32: AUX busy, command 0x%02X not sent", cmd[0]);
    return false;
  }
  drain();
  Serial1.write(cmd, len);
  Serial1.flush();
  return true;
}

uint8_t E32Radio::readAnswer(uint8_t* buf, uint8_t expected, uint32_t timeoutMs) {
  uint8_t got = 0;
  const uint32_t deadline = millis() + timeoutMs;
  while (got < expected && (int32_t)(millis() - deadline) < 0) {
    if (Serial1.available()) {
      buf[got++] = (uint8_t)Serial1.read();
    }
  }
  return got;
}

void E32Radio::drain() {
  while (Serial1.available()) {
    Serial1.read();
  }
  reader_.reset();
}

bool E32Radio::begin() {
  pinMode(kLoraM0Pin, OUTPUT);
  pinMode(kLoraM1Pin, OUTPUT);
  pinMode(kLoraAuxPin, INPUT_PULLUP);
  Serial1.begin(kLoraBaud, SERIAL_8N1);

  // Until Zephyr has brought up the router UART the Bridge falls back to
  // Serial1, so the E32 has already been fed a burst of router traffic on its
  // RXD by the time this runs. drain() below clears our side; the module's side
  // is why M0/M1 need external pull-ups, see the README.
  if (!waitAuxStable(cfg::kAuxStableMs, cfg::kAuxResetTimeoutMs)) {
    logf("e32: module never finished its power-on self-check");
    return false;
  }
  return setMode(E32Mode::Sleep);
}

bool E32Radio::setMode(E32Mode mode) {
  // Manual section 6.1: a mode switch is only valid while AUX is high, and
  // switching into sleep is the documented exception - the module finishes what
  // it has queued and goes dormant ~1 ms later by itself.
  if (mode != E32Mode::Sleep && !waitAuxStable(cfg::kAuxStableMs, cfg::kAuxTimeoutMs)) {
    logf("e32: AUX still busy before the switch to mode %u", (unsigned)mode);
    return false;
  }

  const bool leavingSleep = mode_ == E32Mode::Sleep && mode != E32Mode::Sleep;
  digitalWrite(kLoraM0Pin, ((uint8_t)mode & 0x01) ? HIGH : LOW);
  digitalWrite(kLoraM1Pin, ((uint8_t)mode & 0x02) ? HIGH : LOW);

  // Section 5.6.4 note 4: leaving mode 3 makes the module reload its user
  // parameters with AUX low throughout. That window is exactly what a
  // first-high-sample check would walk straight past.
  delay(cfg::kAuxBusyGuardMs);
  if (!waitAuxStable(cfg::kAuxStableMs,
                     leavingSleep ? cfg::kAuxResetTimeoutMs : cfg::kAuxTimeoutMs)) {
    logf("e32: AUX never idled switching to mode %u", (unsigned)mode);
    return false;
  }
  mode_ = mode;
  drain();
  return true;
}

// Manual section 7.4: C4 makes the module self-check and reload its parameters
// from storage, holding AUX low until it is ready for another instruction.
bool E32Radio::resetModule() {
  const uint8_t cmd[3] = {kCmdReset, kCmdReset, kCmdReset};
  if (!sendCommand(cmd, sizeof(cmd))) {
    return false;
  }
  delay(cfg::kAuxBusyGuardMs);
  if (!waitAuxStable(cfg::kAuxStableMs, cfg::kAuxResetTimeoutMs)) {
    logf("e32: module never came back from the C4 reset");
    return false;
  }
  drain();
  return true;
}

bool E32Radio::readConfig(E32Config* out) {
  if (mode_ != E32Mode::Sleep) {
    return false;
  }
  const uint8_t cmd[3] = {kCmdReadConfig, kCmdReadConfig, kCmdReadConfig};
  if (!sendCommand(cmd, sizeof(cmd))) {
    return false;
  }

  uint8_t cfgBytes[kConfigBytes] = {0};
  const uint8_t got = readAnswer(cfgBytes, kConfigBytes, cfg::kRadioConfigTimeoutMs);
  if (got != kConfigBytes || (cfgBytes[0] != kCmdWriteSaved && cfgBytes[0] != kCmdWriteLive)) {
    logf("e32: config read failed (%u bytes, head 0x%02X)", got, cfgBytes[0]);
    return false;
  }
  out->address = (uint16_t)((uint16_t)cfgBytes[1] << 8 | cfgBytes[2]);
  out->sped = cfgBytes[3];
  out->channel = cfgBytes[4];
  out->option = cfgBytes[5];
  return true;
}

bool E32Radio::ensureConfig(const E32Config& wanted) {
  if (!setMode(E32Mode::Sleep)) {
    return false;
  }
  E32Config current;
  // One retry: if the MCU resets while the module still has traffic in flight,
  // the first C1 C1 C1 can be swallowed and the read times out.
  if (!readConfig(&current)) {
    logf("e32: config read timed out; retrying once");
    delay(cfg::kAuxStableMs);
    drain();
    if (!readConfig(&current)) {
      return false;
    }
  }
  if (current.address == wanted.address && current.sped == wanted.sped &&
      current.channel == wanted.channel && current.option == wanted.option) {
    logf("e32: config ok: addr 0x%04X chan 0x%02X (%u MHz) sped 0x%02X option 0x%02X",
         current.address, current.channel, 862u + current.channel, current.sped, current.option);
    return true;
  }

  logf("e32: config 0x%04X/0x%02X/0x%02X/0x%02X -> 0x%04X/0x%02X/0x%02X/0x%02X", current.address,
       current.sped, current.channel, current.option, wanted.address, wanted.sped, wanted.channel,
       wanted.option);
  const uint8_t write[kConfigBytes] = {kCmdWriteSaved,
                                       (uint8_t)(wanted.address >> 8),
                                       (uint8_t)(wanted.address & 0xFF),
                                       wanted.sped,
                                       wanted.channel,
                                       wanted.option};
  if (!sendCommand(write, sizeof(write))) {
    return false;
  }

  // Section 7 documents no answer to C0, but these modules echo the six bytes
  // back. Worth reading, because it separates a command the module never
  // accepted from one it accepted and failed to store; not worth failing on,
  // because the manual does not promise it.
  uint8_t echo[kConfigBytes] = {0};
  const uint8_t echoGot = readAnswer(echo, kConfigBytes, cfg::kRadioConfigTimeoutMs);
  if (echoGot != kConfigBytes || memcmp(echo, write, kConfigBytes) != 0) {
    logf("e32: C0 echo was %u bytes: %02X %02X %02X %02X %02X %02X", echoGot, echo[0], echo[1],
         echo[2], echo[3], echo[4], echo[5]);
  }

  delay(cfg::kAuxBusyGuardMs);
  if (!waitAuxStable(cfg::kAuxStableMs, cfg::kAuxTimeoutMs)) {
    logf("e32: AUX never idled after the config write");
    return false;
  }

  // Verifying through a C4 reset rather than a plain readback is what makes
  // this a persistence check. A write that reached the live registers but not
  // storage would read back clean and stay clean until the first power cut
  // silently reverted the gateway's address.
  if (!resetModule()) {
    return false;
  }

  E32Config readback;
  if (!readConfig(&readback)) {
    return false;
  }
  if (readback.address != wanted.address || readback.sped != wanted.sped ||
      readback.channel != wanted.channel || readback.option != wanted.option) {
    logf("e32: config write did not persist");
    return false;
  }
  return true;
}

bool E32Radio::sendTo(uint16_t dstAddress, uint8_t dstChannel, const uint8_t* frame, uint8_t len) {
  if (mode_ != E32Mode::Normal) {
    logf("e32: send attempted in mode %u", (unsigned)mode_);
    return false;
  }
  if (len == 0 || cfg::kRoutingBytes + len > cfg::kMaxAirPayload) {
    logf("e32: frame of %u bytes exceeds one air packet", len);
    return false;
  }
  // Transmitting into a module whose buffer has not drained is how frames get
  // silently truncated; refuse rather than write blind.
  if (!waitAuxStable(cfg::kAuxStableMs, cfg::kAuxTimeoutMs)) {
    logf("e32: AUX busy, refusing to transmit");
    return false;
  }

  uint8_t out[cfg::kMaxAirPayload];
  out[0] = (uint8_t)(dstAddress >> 8);
  out[1] = (uint8_t)(dstAddress & 0xFF);
  out[2] = dstChannel;
  memcpy(out + cfg::kRoutingBytes, frame, len);
  Serial1.write(out, cfg::kRoutingBytes + len);
  Serial1.flush();
  return true;
}

E32Rx E32Radio::receive(proto::Frame* out, uint8_t* store, uint8_t storeLen, uint32_t timeoutMs) {
  // The reader deliberately survives across calls. A 49-byte fragment takes
  // ~51 ms to stream in at 9600 baud, so a caller polling in shorter slices
  // would otherwise discard a partial frame on every call and could never
  // assemble anything larger than its own poll window. Stream state is cleared
  // only by drain(), alongside the UART buffer it belongs to.
  bool sawMalformed = false;
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    while (Serial1.available()) {
      if (!reader_.feed((uint8_t)Serial1.read())) {
        continue;
      }
      const uint8_t len = reader_.length();
      if (len > storeLen) {
        sawMalformed = true;
        continue;
      }
      memcpy(store, reader_.buffer(), len);
      const proto::Decode result = proto::decode(store, len, out);
      if (result == proto::Decode::Ok) {
        return E32Rx::Ok;
      }
      sawMalformed = true;
    }
    delay(1);
  }
  return sawMalformed ? E32Rx::Malformed : E32Rx::Timeout;
}

}  // namespace gw
