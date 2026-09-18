// Phase 1 bench measurements. Two assumptions carry the rest of the firmware and
// neither is answerable from a datasheet:
//
//   1. The E32 manual gives 58 bytes as the maximum single air package, but does
//      not say whether the three fixed-transmission routing bytes count against
//      it. cfg::kMaxAirPayload assumes they do, which is the conservative
//      reading; this sweeps write sizes and shows where the module actually
//      starts sub-packing.
//   2. millis() does not survive deep sleep and esp_timer_get_time()'s behaviour
//      across it is not documented. The whole transmission-window schedule is
//      anchored to esp_rtc_get_time_us(), so it must be shown monotonic first.

#include <Arduino.h>
#include <esp_sleep.h>

#include "app_config.hpp"
#include "board_pins.h"
#include "device_id.hpp"
#include "e32_radio.h"
#include "esp32/rtc.h"

namespace {

constexpr uint32_t kSleepTestS = 20;
// A fresh air package cannot follow the previous byte immediately; at 2.4 kbps
// the inter-package gap is far longer than the ~1 ms between bytes at 9600 baud.
constexpr uint32_t kGapThresholdMs = 15;
constexpr uint32_t kQuietMs = 1500;

const uint8_t kSweep[] = {40, 52, 55, 58, 61, 64, 80, 120};

RTC_DATA_ATTR uint64_t g_rtcBeforeSleep;
RTC_DATA_ATTR uint32_t g_sleepMagic;
constexpr uint32_t kSleepMagic = 0x51EEB1EDUL;

E32Radio radio;

void reportRtc() {
  const uint64_t now = esp_rtc_get_time_us();
  Serial.printf("\nRTC counter now: %llu us (%lu s)\n", (unsigned long long)now,
                (unsigned long)(now / 1000000ULL));
  if (g_sleepMagic != kSleepMagic) {
    Serial.println("no prior sleep recorded (cold start)");
    return;
  }
  g_sleepMagic = 0;
  if (now < g_rtcBeforeSleep) {
    Serial.printf("FAIL: counter went BACKWARDS by %llu us across deep sleep\n",
                  (unsigned long long)(g_rtcBeforeSleep - now));
    return;
  }
  const uint64_t delta = now - g_rtcBeforeSleep;
  const double seconds = (double)delta / 1e6;
  const double error = (seconds - (double)kSleepTestS) / (double)kSleepTestS * 100.0;
  Serial.printf("slept %.3f s against a requested %lu s -> %+.2f %% (internal 150 kHz RC)\n",
                seconds, (unsigned long)kSleepTestS, error);
  Serial.println(seconds > 1.0 ? "PASS: esp_rtc_get_time_us() advances across deep sleep"
                               : "FAIL: counter did not advance");
}

void sleepTest() {
  g_rtcBeforeSleep = esp_rtc_get_time_us();
  g_sleepMagic = kSleepMagic;
  Serial.printf("sleeping %lu s...\n", (unsigned long)kSleepTestS);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)kSleepTestS * 1000000ULL);
  esp_deep_sleep_start();
}

bool bringUpRadio(uint16_t ownAddress) {
  if (!radio.begin()) {
    Serial.println("radio begin failed");
    return false;
  }
  const E32Config wanted = {ownAddress, cfg::kLoraSped, cfg::kLoraChannel, cfg::kLoraOption};
  if (!radio.ensureConfig(wanted)) {
    Serial.println("radio config failed");
    return false;
  }
  return radio.setMode(E32Mode::Normal);
}

// Writes past cfg::kMaxAirPayload on purpose, so it goes around E32Radio::sendTo,
// which refuses exactly that.
void sendRaw(uint16_t dst, uint8_t len) {
  uint8_t buf[3 + 255];
  buf[0] = (uint8_t)(dst >> 8);
  buf[1] = (uint8_t)(dst & 0xFF);
  buf[2] = cfg::kLoraChannel;
  for (uint8_t i = 0; i < len; ++i) {
    buf[3 + i] = i;
  }
  while (digitalRead(PIN_LORA_AUX) == LOW) {
    delay(1);
  }
  delay(cfg::kAuxSettleMs);
  Serial2.write(buf, (size_t)(3 + len));
  Serial2.flush();
}

void runSender() {
  if (!bringUpRadio(kBuiltInEndpointId)) {
    return;
  }
  Serial.printf("sender 0x%04X -> 0x%04X. Watch the receiver's console.\n", kBuiltInEndpointId,
                cfg::kGatewayId);
  for (uint8_t i = 0; i < sizeof(kSweep); ++i) {
    Serial.printf("payload %u bytes (UART write %u including routing)\n", kSweep[i],
                  kSweep[i] + 3);
    sendRaw(cfg::kGatewayId, kSweep[i]);
    delay(4000);
  }
  Serial.println("sweep done");
}

void runReceiver() {
  if (!bringUpRadio(cfg::kGatewayId)) {
    return;
  }
  Serial.printf("receiver 0x%04X listening. A gap over %lu ms marks a new air package.\n",
                cfg::kGatewayId, (unsigned long)kGapThresholdMs);

  uint16_t total = 0;
  uint16_t runLen = 0;
  uint8_t runs = 0;
  uint16_t runSizes[16];
  uint32_t lastByteMs = 0;

  while (true) {
    if (Serial2.available()) {
      const uint32_t now = millis();
      Serial2.read();
      if (total > 0 && (now - lastByteMs) > kGapThresholdMs) {
        if (runs < 16) {
          runSizes[runs] = runLen;
        }
        ++runs;
        runLen = 0;
      }
      ++total;
      ++runLen;
      lastByteMs = now;
      continue;
    }

    if (total > 0 && (millis() - lastByteMs) > kQuietMs) {
      if (runs < 16) {
        runSizes[runs] = runLen;
      }
      ++runs;
      Serial.printf("received %u bytes in %u air package(s):", total, runs);
      for (uint8_t i = 0; i < runs && i < 16; ++i) {
        Serial.printf(" %u", runSizes[i]);
      }
      Serial.println();
      total = 0;
      runLen = 0;
      runs = 0;
    }

    if (Serial.available()) {
      Serial.read();
      Serial.println("receiver stopped");
      return;
    }
    delay(1);
  }
}

void menu() {
  Serial.println("\nkeys:  s = deep sleep 20 s and re-check the RTC counter");
  Serial.println("       a = air-size sweep, sender role");
  Serial.println("       b = air-size sweep, receiver role (any key stops it)");
}

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(300);
  Serial.println("\nSILOMETER probe - Phase 1 bench measurements");
  Serial.printf("compiled budget: kMaxAirPayload %u, routing %u, frame <= %u, %u fragments\n",
                cfg::kMaxAirPayload, cfg::kRoutingBytes, proto::kMaxFrameBytes,
                proto::kFragmentCount);
  reportRtc();
  menu();
}

void loop() {
  if (!Serial.available()) {
    delay(20);
    return;
  }
  switch (Serial.read()) {
    case 's': sleepTest(); break;
    case 'a': runSender(); break;
    case 'b': runReceiver(); break;
    default: menu(); break;
  }
}
