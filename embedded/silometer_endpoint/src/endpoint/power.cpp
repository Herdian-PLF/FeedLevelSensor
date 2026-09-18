#include "power.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "board_pins.h"
#include "esp32/rtc.h"
#include "log.hpp"

namespace {

constexpr char kTag[] = "pwr";

// Pins whose level must survive deep sleep: the radio's mode select, its UART
// idle level, and the sensor enable. gpio_deep_sleep_hold_en() covers digital
// pads on its own; the per-pin gpio_hold_en() also covers IO25, which is an RTC
// pad, and costs nothing on the others.
const gpio_num_t kHeldPins[] = {
    (gpio_num_t)PIN_LORA_M0,
    (gpio_num_t)PIN_LORA_M1,
    (gpio_num_t)PIN_LORA_TX,
    (gpio_num_t)PIN_TOF_EN,
};

}  // namespace

void powerEarlyInit() {
  // Order matters, and the IDF documents exactly this trap: a held pin reverts
  // to its default (input) mode when the hold is released, so releasing first
  // would float M0/M1 and could kick the radio out of sleep into a transmitting
  // mode, or float the sensor enable.
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  pinMode(PIN_LORA_TX, OUTPUT);
  pinMode(PIN_TOF_EN, OUTPUT);
  digitalWrite(PIN_LORA_M0, HIGH);
  digitalWrite(PIN_LORA_M1, HIGH);
  digitalWrite(PIN_LORA_TX, HIGH);
  digitalWrite(PIN_TOF_EN, LOW);

  for (gpio_num_t pin : kHeldPins) {
    gpio_hold_dis(pin);
  }
  gpio_deep_sleep_hold_dis();

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
}

uint64_t rtcNowUs() { return esp_rtc_get_time_us(); }

uint32_t resetReasonCode() { return (uint32_t)esp_reset_reason(); }

bool wokeFromTimer() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }

bool resetWasAbnormal() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_BROWNOUT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

void enterDeepSleep(uint64_t durationUs) {
  digitalWrite(PIN_STATUS_LED, LOW);

  // Serial2.end() leaves IO17 back under GPIO control but not necessarily as an
  // output, and digitalWrite on an input pin only moves the pull-up. Reassert
  // the direction before the level on every held pin.
  //
  // The radio sits in mode 3 at ~4 uA for the whole sleep, and IO17 idles high
  // so a floating line cannot look like a config write to a module that accepts
  // one in exactly this mode.
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  pinMode(PIN_LORA_TX, OUTPUT);
  pinMode(PIN_TOF_EN, OUTPUT);
  digitalWrite(PIN_LORA_M0, HIGH);
  digitalWrite(PIN_LORA_M1, HIGH);
  digitalWrite(PIN_LORA_TX, HIGH);
  digitalWrite(PIN_TOF_EN, LOW);

  pinMode(PIN_TOF_SDA, INPUT);
  pinMode(PIN_TOF_SCL, INPUT);

  for (gpio_num_t pin : kHeldPins) {
    gpio_hold_en(pin);
  }
  gpio_deep_sleep_hold_en();

  LOG_I(kTag, "deep sleep for %lu s", (unsigned long)(durationUs / 1000000ULL));
  Serial.flush();
  esp_sleep_enable_timer_wakeup(durationUs);
  esp_deep_sleep_start();
}
