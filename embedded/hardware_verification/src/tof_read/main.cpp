// ESP32 <-> TMF8829 measurement readout for the silometer perfboard prototype.
//
// Downloads the sensor's application firmware over I2C, runs the vendor 8x8
// long-range preconfiguration and prints one frame every 5 seconds. The driver
// itself is ams-OSRAM's, fetched by tools/fetch_vendor.sh; only the shim is ours.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "board_pins.h"
#include "tmf8829_shim.h"

#include "tmf8829.h"
#include "tmf8829_image.h"

namespace {

const uint32_t PRINT_PERIOD_MS = 5000;
const uint8_t GRID = 8;

#ifndef TOF_PRECONFIG
#define TOF_PRECONFIG TMF8829_CMD_STAT__cmd_stat__CMD_LOAD_CFG_8X8_LONG_RANGE
#endif

// AN001096: the SNR byte is linear up to 40 and exponentially companded above it,
// so the raw byte understates strong returns by more than an order of magnitude.
uint32_t decodeConfidence(uint8_t snr) {
  const uint8_t breakpoint = 40;
  if (snr <= breakpoint) {
    return snr;
  }
  return (uint32_t)(breakpoint * powf(1.053676f, (float)(snr - breakpoint)));
}

tmf8829Driver driver;
bool running = false;
uint32_t lastPrint = 0;

bool bringUp() {
  pinMode(PIN_TOF_EN, OUTPUT);
  pinMode(PIN_TOF_INT, INPUT_PULLUP);
  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL, TOF_I2C_HZ);

  tmf8829Initialise(&driver);
  driver.i2cSlaveAddress = TOF_I2C_ADDR;
  tmf8829SetLogLevel(&driver, 0);

  tmf8829Disable(&driver);
  delay(10);
  tmf8829Enable(&driver);
  if (!tmf8829IsCpuReady(&driver, 100)) {
    Serial.println("FAIL: CPU not ready after enable");
    return false;
  }

  if (tmf8829DownloadFirmware(&driver, tmf8829_image_start, tmf8829_image,
                              (int32_t)tmf8829_image_length, 1) != BL_SUCCESS_OK) {
    Serial.println("FAIL: firmware download");
    return false;
  }

  uint8_t appId = 0;
  rxReg(&driver, driver.i2cSlaveAddress, TMF8829_COM_REG_APP_ID, 1, &appId);
  if (appId != TMF8829_COM_APP_ID__application) {
    Serial.printf("FAIL: APP_ID=0x%02X, expected 0x01\n", appId);
    return false;
  }

  if (tmf8829Command(&driver, TOF_PRECONFIG) != APP_SUCCESS_OK) {
    Serial.println("FAIL: 8x8 preconfig");
    return false;
  }

  if (tmf8829GetConfiguration(&driver) == APP_SUCCESS_OK &&
      driver.config[TMF8829_CFG_ALG_DISTANCE - TMF8829_CFG_PERIOD_MS_LSB] != 0x01) {
    Serial.println("WARN: ALG_DISTANCE is not the default mode - distances are not 0.25 mm steps");
  }

  tmf8829ClrAndEnableInterrupts(&driver, TMF8829_APP_INT_RESULTS);
  if (tmf8829StartMeasurement(&driver) != APP_SUCCESS_OK) {
    Serial.println("FAIL: start measurement");
    return false;
  }

  Serial.println("TMF8829 running: 8x8 long range, frame every 5 s");
  return true;
}

void printFrame() {
  if (!tofFrame.complete) {
    Serial.println("no frame yet");
    return;
  }
  uint32_t sum = 0;
  uint16_t valid = 0;
  uint16_t lo = 0xFFFF;
  uint16_t hi = 0;

  Serial.printf("\nframe %lu  T=%uC  zones=%u\n", (unsigned long)tofFrame.frameNumber,
                tofFrame.temperature, tofFrame.zones);

  for (uint8_t row = 0; row < GRID; ++row) {
    for (uint8_t col = 0; col < GRID; ++col) {
      const uint8_t i = row * GRID + col;
      if (i >= tofFrame.zones) {
        break;
      }
      const uint16_t d = tofFrame.distanceMm[i];
      const uint8_t rawSnr = tofFrame.snr[i];
      Serial.printf(" %5u/%-4lu", d, (unsigned long)decodeConfidence(rawSnr));
      if (rawSnr > 0) {
        sum += d;
        ++valid;
        lo = min(lo, d);
        hi = max(hi, d);
      }
    }
    Serial.println();
  }
  if (valid > 0) {
    Serial.printf("valid %u/%u  min %u  max %u  mean %lu  (mm, dist/confidence)\n", valid,
                  tofFrame.zones, lo, hi, (unsigned long)(sum / valid));
  } else {
    Serial.printf("valid 0/%u - nothing in range\n", tofFrame.zones);
  }
}

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(500);
  Serial.println("\nSILOMETER - TMF8829 measurement readout");
  running = bringUp();
}

void loop() {
  if (!running) {
    delay(1000);
    return;
  }
  if (tmf8829GetAndClrInterrupts(&driver, TMF8829_APP_INT_RESULTS) & TMF8829_APP_INT_RESULTS) {
    tmf8829ReadResults(&driver);
  }
  if (millis() - lastPrint >= PRINT_PERIOD_MS) {
    lastPrint = millis();
    printFrame();
  }
}
