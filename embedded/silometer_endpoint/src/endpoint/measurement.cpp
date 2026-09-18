#include "measurement.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "board_pins.h"
#include "log.hpp"
#include "tmf8829.h"
#include "tmf8829_image.h"
#include "tmf8829_shim.h"

namespace {

constexpr char kTag[] = "tof";

#ifndef TOF_PRECONFIG
#define TOF_PRECONFIG TMF8829_CMD_STAT__cmd_stat__CMD_LOAD_CFG_8X8_LONG_RANGE
#endif

tmf8829Driver driver;

uint16_t samples[cfg::kMaxTofFrames][cfg::kZoneCount];
uint8_t sampleSnr[cfg::kMaxTofFrames][cfg::kZoneCount];

// Picks the median sample by distance and returns that sample's index, so the
// reported SNR belongs to the distance actually reported rather than to a
// different frame. Samples with no target are excluded, not treated as zero.
bool medianSample(uint8_t zone, uint8_t frames, uint16_t* distance, uint8_t* snr) {
  uint8_t order[cfg::kMaxTofFrames];
  uint8_t n = 0;
  for (uint8_t f = 0; f < frames; ++f) {
    if (samples[f][zone] != 0) {
      order[n++] = f;
    }
  }
  if (n == 0) {
    *distance = 0;
    *snr = 0;
    return false;
  }
  for (uint8_t i = 1; i < n; ++i) {
    const uint8_t key = order[i];
    int8_t j = (int8_t)(i - 1);
    while (j >= 0 && samples[order[j]][zone] > samples[key][zone]) {
      order[j + 1] = order[j];
      --j;
    }
    order[j + 1] = key;
  }
  const uint8_t pick = order[n / 2];
  *distance = samples[pick][zone];
  *snr = sampleSnr[pick][zone];
  return true;
}

}  // namespace

bool TofSensor::begin() {
  pinMode(PIN_TOF_EN, OUTPUT);
  pinMode(PIN_TOF_INT, INPUT_PULLUP);
  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL, TOF_I2C_HZ);

  tmf8829Initialise(&driver);
  driver.i2cSlaveAddress = TOF_I2C_ADDR;
  tmf8829SetLogLevel(&driver, 0);

  tmf8829Disable(&driver);
  delay(10);
  tmf8829Enable(&driver);
  if (!tmf8829IsCpuReady(&driver, cfg::kTofCpuReadyTimeoutMs)) {
    LOG_E(kTag, "CPU not ready after enable");
    return false;
  }

  if (tmf8829DownloadFirmware(&driver, tmf8829_image_start, tmf8829_image,
                              (int32_t)tmf8829_image_length, 1) != BL_SUCCESS_OK) {
    LOG_E(kTag, "firmware download failed");
    return false;
  }

  uint8_t appId = 0;
  rxReg(&driver, driver.i2cSlaveAddress, TMF8829_COM_REG_APP_ID, 1, &appId);
  if (appId != TMF8829_COM_APP_ID__application) {
    LOG_E(kTag, "APP_ID 0x%02X, expected 0x01", appId);
    return false;
  }

  if (tmf8829Command(&driver, TOF_PRECONFIG) != APP_SUCCESS_OK) {
    LOG_E(kTag, "8x8 preconfig failed");
    return false;
  }

  if (tmf8829GetConfiguration(&driver) == APP_SUCCESS_OK &&
      driver.config[TMF8829_CFG_ALG_DISTANCE - TMF8829_CFG_PERIOD_MS_LSB] != 0x01) {
    LOG_W(kTag, "ALG_DISTANCE is not the default mode - distances are not 0.25 mm steps");
  }

  tmf8829ClrAndEnableInterrupts(&driver, TMF8829_APP_INT_RESULTS);
  if (tmf8829StartMeasurement(&driver) != APP_SUCCESS_OK) {
    LOG_E(kTag, "start measurement failed");
    return false;
  }
  started_ = true;
  LOG_I(kTag, "running 8x8 long range");
  return true;
}

bool TofSensor::awaitFrame(uint32_t timeoutMs) {
  tofFrameReady = false;
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (tmf8829GetAndClrInterrupts(&driver, TMF8829_APP_INT_RESULTS) & TMF8829_APP_INT_RESULTS) {
      tmf8829ReadResults(&driver);
      if (tofFrameReady) {
        return true;
      }
    }
    delay(1);
  }
  return false;
}

bool TofSensor::measure(uint8_t frames, Reading* out) {
  memset(out, 0, sizeof(*out));
  if (!started_) {
    return false;
  }
  if (frames > cfg::kMaxTofFrames) {
    frames = cfg::kMaxTofFrames;
  }

  uint8_t collected = 0;
  for (uint8_t f = 0; f < frames; ++f) {
    if (!awaitFrame(cfg::kTofFrameTimeoutMs)) {
      LOG_W(kTag, "frame %u timed out", f);
      break;
    }
    const uint8_t zones =
        tofFrame.zones < cfg::kZoneCount ? tofFrame.zones : (uint8_t)cfg::kZoneCount;
    memset(samples[collected], 0, sizeof(samples[collected]));
    memset(sampleSnr[collected], 0, sizeof(sampleSnr[collected]));
    for (uint8_t z = 0; z < zones; ++z) {
      samples[collected][z] = tofFrame.distanceMm[z];
      sampleSnr[collected][z] = tofFrame.snr[z];
    }
    out->frameNumber = tofFrame.frameNumber;
    out->temperatureC = tofFrame.temperature;
    out->zones = zones;
    ++collected;
  }

  if (collected == 0) {
    LOG_E(kTag, "no frames collected");
    return false;
  }

  out->validZones = 0;
  for (uint8_t z = 0; z < cfg::kZoneCount; ++z) {
    if (medianSample(z, collected, &out->distanceMm[z], &out->snr[z])) {
      ++out->validZones;
    }
  }
  out->valid = true;
  LOG_I(kTag, "frame %lu: %u/%u zones, %u C, median of %u",
        (unsigned long)out->frameNumber, out->validZones, out->zones, out->temperatureC, collected);
  return out->validZones >= cfg::kMinValidZones;
}

void TofSensor::powerDown() {
  if (started_) {
    tmf8829StopMeasurement(&driver);
    tmf8829DisableInterrupts(&driver, 0xFF);
    started_ = false;
  }
  tmf8829Disable(&driver);
  Wire.end();
  // EN low is a chip enable, not a load switch: the shield's own regulator and
  // its TXS0104 keep drawing. Quantifying that residual needs a meter.
  LOG_D(kTag, "powered down");
}
