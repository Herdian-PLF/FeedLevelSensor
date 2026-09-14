#include "tmf8829_shim.h"

#include <Wire.h>

#include "tmf8829.h"

// ESP32 Wire buffers 128 bytes, so a transfer is split below that. The FIFO
// register streams rather than auto-increments, so its address must NOT advance
// between chunks; every other register does auto-increment.
// AN001096 section 4.6.4.2: in the default distance mode (TMF8829_CFG_ALG_DISTANCE
// = 0x01) the sensor reports distance in 0.25 mm steps, so raw counts are
// quartered. The vendor Python driver does the same in pixelResultsToMM().
static const uint16_t DISTANCE_STEPS_PER_MM = 4;

static const uint16_t I2C_CHUNK = 64;
static const uint8_t REG_FIFO = 0xFF;

volatile bool tofFrameReady = false;
TofFrame tofFrame = {};

static uint8_t framePixelSize = 0;
static uint8_t frameLayout = 0;
static uint8_t frameZones = 0;

void delayInMicroseconds(uint32_t wait) { delayMicroseconds(wait); }

uint32_t getSysTick(void) { return micros(); }

uint8_t readProgramMemoryByte(uint32_t address) { return *(const uint8_t*)address; }

void enablePinHigh(void* dptr) {
  (void)dptr;
  digitalWrite(ENABLE_PIN, HIGH);
}

void enablePinLow(void* dptr) {
  (void)dptr;
  digitalWrite(ENABLE_PIN, LOW);
}

int8_t txReg(void* dptr, uint8_t slaveAddr, uint8_t regAddr, uint16_t toTx, const uint8_t* txData) {
  (void)dptr;
  uint16_t sent = 0;
  do {
    uint16_t chunk = toTx - sent;
    if (chunk > I2C_CHUNK) {
      chunk = I2C_CHUNK;
    }
    Wire.beginTransmission(slaveAddr);
    Wire.write(regAddr == REG_FIFO ? regAddr : (uint8_t)(regAddr + sent));
    if (chunk > 0) {
      Wire.write(txData + sent, chunk);
    }
    if (Wire.endTransmission() != 0) {
      return -1;
    }
    sent += chunk;
  } while (sent < toTx);
  return I2C_SUCCESS;
}

int8_t rxReg(void* dptr, uint8_t slaveAddr, uint8_t regAddr, uint16_t toRx, uint8_t* rxData) {
  (void)dptr;
  uint16_t got = 0;
  while (got < toRx) {
    uint16_t chunk = toRx - got;
    if (chunk > I2C_CHUNK) {
      chunk = I2C_CHUNK;
    }
    Wire.beginTransmission(slaveAddr);
    Wire.write(regAddr == REG_FIFO ? regAddr : (uint8_t)(regAddr + got));
    if (Wire.endTransmission(false) != 0) {
      return -1;
    }
    if (Wire.requestFrom((int)slaveAddr, (int)chunk) != (int)chunk) {
      return -1;
    }
    for (uint16_t i = 0; i < chunk; ++i) {
      rxData[got + i] = (uint8_t)Wire.read();
    }
    got += chunk;
  }
  return I2C_SUCCESS;
}

void printChar(char c) { Serial.print(c); }
void printInt(int32_t i) { Serial.print(i); }
void printUint(uint32_t i) { Serial.print(i); }
void printUintHex(uint32_t i) { Serial.print(i, HEX); }
void printStr(char* str) { Serial.print(str); }
void printConstStr(const char* str) { Serial.print(str); }
void printLn(void) { Serial.println(); }

void handleReceivedFrameHeaderData(void* dptr, uint8_t* data) {
  (void)dptr;
  frameLayout = data[TMF8829_PRE_HEADER_SIZE + 1];
  framePixelSize = tmf8829GetPixelSize(frameLayout);
  frameZones = 0;
  tofFrame.frameNumber = tmf8829GetUint32(data + TMF8829_PRE_HEADER_SIZE + 4);
  tofFrame.temperature = data[TMF8829_PRE_HEADER_SIZE + 8];

}

void handleReceivedResultData(void* dptr, uint8_t* data, uint16_t size) {
  (void)dptr;
  if (framePixelSize == 0) {
    return;
  }
  // Noise and crosstalk, when present, precede the peaks in every pixel.
  uint8_t peakOffset = 0;
  if (frameLayout & TMF8829_CFG_RESULT_FORMAT_NOISE_STRENGTH_MASK) {
    peakOffset += 2;
  }
  if (frameLayout & TMF8829_CFG_RESULT_FORMAT_XTALK_MASK) {
    peakOffset += 2;
  }
  // The last chunk carries the frame footer; capping at the zone count drops it.
  for (uint16_t i = 0; i + framePixelSize <= size && frameZones < TOF_ZONES; i += framePixelSize) {
    tofFrame.distanceMm[frameZones] =
        tmf8829GetUint16(data + i + peakOffset) / DISTANCE_STEPS_PER_MM;
    tofFrame.snr[frameZones] = data[i + peakOffset + 2];
    ++frameZones;
  }
}

void handleReceivedResultDataEnd(void* dptr) {
  (void)dptr;
  tofFrame.zones = frameZones;
  tofFrame.complete = frameZones > 0;
  tofFrameReady = tofFrame.complete;
}

void handleReceivedHistogramData(void* dptr, uint8_t* data, uint16_t size) {
  (void)dptr;
  (void)data;
  (void)size;
}

void handleReceivedHistogramDataEnd(void* dptr) { (void)dptr; }
