#pragma once

// Net names and pin numbers come from silometer.kicad_sch/perfboard prototype

#include <stdint.h>

// TMF8829_EVM_EB_SHIELD
static const uint8_t PIN_TOF_SDA = 21;  // shield CN4-9
static const uint8_t PIN_TOF_SCL = 22;  // shield CN4-10
static const uint8_t PIN_TOF_EN = 25;   // shield CN6-7, active high
static const uint8_t PIN_TOF_INT = 26;  // shield CN6-3, active low, open-drain

// The shield's level shifter does not carry pull-ups; 4k7 to 3V3 is the fallback.
static const uint32_t TOF_I2C_HZ = 100000;      // TFM8829 datasheet section 5; max value
static const uint8_t TOF_I2C_ADDR = 0x41;       // TFM8829 datasheet section 8.1.15

// EBYTE E32-900T20D. Net names are written from the module's point of view,
// so LORA_RXD carries ESP32 transmit data.
static const uint8_t PIN_LORA_TX = 17;   // ESP32 TX2 -> E32 pin 3 RXD
static const uint8_t PIN_LORA_RX = 16;   // ESP32 RX2 <- E32 pin 4 TXD
static const uint8_t PIN_LORA_M0 = 18;   // E32 pin 1
static const uint8_t PIN_LORA_M1 = 19;   // E32 pin 2
static const uint8_t PIN_LORA_AUX = 4;   // E32 pin 5, high = idle

static const uint32_t LORA_DEFAULT_BAUD = 9600;

// UART0 (IO1/IO3) is deliberately left to the USB console so flashing and
// monitoring keep working while UART2 drives the radio.
static const uint32_t CONSOLE_BAUD = 115200;
