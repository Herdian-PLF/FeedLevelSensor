#include "e32_radio.h"

#include <Arduino.h>
#include <string.h>

#include "app_config.hpp"
#include "board_pins.h"
#include "log.hpp"

namespace {

constexpr char kTag[] = "e32";
constexpr uint8_t kCmdReadConfig = 0xC1;
constexpr uint8_t kCmdWriteSaved = 0xC0;
constexpr uint8_t kConfigBytes = 6;

}  // namespace

bool E32Radio::waitAux(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (digitalRead(PIN_LORA_AUX) == HIGH) {
      // The manual gives 1 ms from the AUX rising edge to the mode actually
      // being in effect; anything sent inside that window can be lost.
      delay(cfg::kAuxSettleMs);
      return true;
    }
    delay(1);  // never busy-spin: the task watchdog is left enabled on purpose
  }
  return false;
}

void E32Radio::drain() {
  while (Serial2.available()) {
    Serial2.read();
  }
  reader_.reset();
}

bool E32Radio::begin() {
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  pinMode(PIN_LORA_AUX, INPUT_PULLUP);
  Serial2.begin(LORA_DEFAULT_BAUD, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);
  return setMode(E32Mode::Sleep);
}

void E32Radio::end() { Serial2.end(); }

bool E32Radio::setMode(E32Mode mode) {
  digitalWrite(PIN_LORA_M0, ((uint8_t)mode & 0x01) ? HIGH : LOW);
  digitalWrite(PIN_LORA_M1, ((uint8_t)mode & 0x02) ? HIGH : LOW);
  delay(cfg::kModeSwitchSettleMs);
  if (!waitAux(cfg::kAuxTimeoutMs)) {
    LOG_E(kTag, "AUX never idled switching to mode %u", (unsigned)mode);
    return false;
  }
  mode_ = mode;
  drain();
  return true;
}

bool E32Radio::readConfig(E32Config* out) {
  if (mode_ != E32Mode::Sleep) {
    return false;
  }
  drain();
  const uint8_t cmd[3] = {kCmdReadConfig, kCmdReadConfig, kCmdReadConfig};
  Serial2.write(cmd, sizeof(cmd));
  Serial2.flush();

  uint8_t cfgBytes[kConfigBytes] = {0};
  uint8_t got = 0;
  const uint32_t deadline = millis() + cfg::kRadioConfigTimeoutMs;
  while (got < kConfigBytes && (int32_t)(millis() - deadline) < 0) {
    if (Serial2.available()) {
      cfgBytes[got++] = (uint8_t)Serial2.read();
    }
  }
  if (got != kConfigBytes || (cfgBytes[0] != kCmdWriteSaved && cfgBytes[0] != 0xC2)) {
    LOG_E(kTag, "config read failed (%u bytes, head 0x%02X)", got, cfgBytes[0]);
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
  // One retry: if the ESP32 resets while the module still has traffic in flight,
  // the first C1 C1 C1 can be swallowed and the read times out. Observed once on
  // a cold start, and losing a whole cycle to it is not worth the saved 20 lines.
  if (!readConfig(&current)) {
    LOG_W(kTag, "config read timed out; retrying once");
    delay(cfg::kModeSwitchSettleMs);
    drain();
    if (!readConfig(&current)) {
      return false;
    }
  }
  if (current.address == wanted.address && current.sped == wanted.sped &&
      current.channel == wanted.channel && current.option == wanted.option) {
    LOG_I(kTag, "config ok: addr 0x%04X chan 0x%02X (%u MHz) sped 0x%02X option 0x%02X",
          current.address, current.channel, 862u + current.channel, current.sped, current.option);
    return true;
  }

  LOG_W(kTag, "config 0x%04X/0x%02X/0x%02X/0x%02X -> 0x%04X/0x%02X/0x%02X/0x%02X", current.address,
        current.sped, current.channel, current.option, wanted.address, wanted.sped, wanted.channel,
        wanted.option);
  const uint8_t write[kConfigBytes] = {kCmdWriteSaved,
                                       (uint8_t)(wanted.address >> 8),
                                       (uint8_t)(wanted.address & 0xFF),
                                       wanted.sped,
                                       wanted.channel,
                                       wanted.option};
  Serial2.write(write, sizeof(write));
  Serial2.flush();
  if (!waitAux(cfg::kAuxTimeoutMs)) {
    return false;
  }
  delay(100);
  drain();

  E32Config readback;
  if (!readConfig(&readback)) {
    return false;
  }
  if (readback.address != wanted.address || readback.sped != wanted.sped ||
      readback.channel != wanted.channel || readback.option != wanted.option) {
    LOG_E(kTag, "config write did not stick");
    return false;
  }
  return true;
}

bool E32Radio::sendTo(uint16_t dstAddress, uint8_t dstChannel, const uint8_t* frame, uint8_t len) {
  if (mode_ != E32Mode::Normal) {
    LOG_E(kTag, "send attempted in mode %u", (unsigned)mode_);
    return false;
  }
  if (len == 0 || cfg::kRoutingBytes + len > cfg::kMaxAirPayload) {
    LOG_E(kTag, "frame of %u bytes exceeds one air packet", len);
    return false;
  }
  // Transmitting into a module whose buffer has not drained is how frames get
  // silently truncated; refuse rather than write blind.
  if (!waitAux(cfg::kAuxTimeoutMs)) {
    LOG_E(kTag, "AUX busy, refusing to transmit");
    return false;
  }

  uint8_t out[cfg::kMaxAirPayload];
  out[0] = (uint8_t)(dstAddress >> 8);
  out[1] = (uint8_t)(dstAddress & 0xFF);
  out[2] = dstChannel;
  memcpy(out + cfg::kRoutingBytes, frame, len);
  Serial2.write(out, cfg::kRoutingBytes + len);
  Serial2.flush();
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
    while (Serial2.available()) {
      if (!reader_.feed((uint8_t)Serial2.read())) {
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
      LOG_D(kTag, "discarded frame, decode %u", (unsigned)result);
      sawMalformed = true;
    }
    delay(1);
  }
  return sawMalformed ? E32Rx::Malformed : E32Rx::Timeout;
}

void E32Radio::sleep() {
  setMode(E32Mode::Sleep);
  Serial2.end();
}
