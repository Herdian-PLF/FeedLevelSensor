#pragma once

// ESP32 port of the ams-OSRAM TMF8829 driver shim. The vendor shim targets the
// Arduino Uno; this replaces it and also carries the result callbacks, which the
// vendor places here too. Only what vendor/tmf8829/tmf8829.c actually calls is
// declared - the Uno's UART command interpreter is not ported.

#include <Arduino.h>
#include <pgmspace.h>
#include <stdint.h>

#include "board_pins.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DATA_BUFFER_SIZE 500
#define ARDUINO_MAX_I2C_TRANSFER 32

#define ENABLE_PIN PIN_TOF_EN
#define INTERRUPT_PIN PIN_TOF_INT
#define USE_INTERRUPT_TO_TRIGGER_READ 0

#define HOST_TICKS_PER_1000_US 1000
#define TMF8829_TICKS_PER_1000_US 125

#define I2C_SUCCESS 0

#define PTR_TO_UINT(ptr) ((intptr_t)(ptr))

#define PRINT_CHAR(c) printChar(c)
#define PRINT_INT(i) printInt(i)
#define PRINT_UINT(i) printUint(i)
#define PRINT_UINT_HEX(i) printUintHex(i)
#define PRINT_STR(str) printStr((char*)str)
#define PRINT_CONST_STR(str) printConstStr((const char*)str)
#define PRINT_LN() printLn()
#define SEPARATOR ','

void delayInMicroseconds(uint32_t wait);
uint32_t getSysTick(void);
uint8_t readProgramMemoryByte(uint32_t address);

void enablePinHigh(void* dptr);
void enablePinLow(void* dptr);

int8_t txReg(void* dptr, uint8_t slaveAddr, uint8_t regAddr, uint16_t toTx, const uint8_t* txData);
int8_t rxReg(void* dptr, uint8_t slaveAddr, uint8_t regAddr, uint16_t toRx, uint8_t* rxData);

void printChar(char c);
void printInt(int32_t i);
void printUint(uint32_t i);
void printUintHex(uint32_t i);
void printStr(char* str);
void printConstStr(const char* str);
void printLn(void);

void handleReceivedFrameHeaderData(void* dptr, uint8_t* data);
void handleReceivedResultData(void* dptr, uint8_t* data, uint16_t size);
void handleReceivedResultDataEnd(void* dptr);
void handleReceivedHistogramData(void* dptr, uint8_t* data, uint16_t size);
void handleReceivedHistogramDataEnd(void* dptr);

#if defined(__cplusplus)
}
#endif

// Latest complete frame, filled by the result callbacks. C++ only: the vendor
// driver is C and never touches it.
#define TOF_ZONES 64

#if defined(__cplusplus)

struct TofFrame {
  uint16_t distanceMm[TOF_ZONES];
  uint8_t snr[TOF_ZONES];  // raw byte; log-companded above 40, see decodeConfidence()
  uint32_t frameNumber;
  uint8_t temperature;
  uint8_t zones;
  bool complete;
};

extern volatile bool tofFrameReady;
extern TofFrame tofFrame;

#endif
