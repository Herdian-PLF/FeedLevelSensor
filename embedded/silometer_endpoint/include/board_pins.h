#pragma once

// Net names and pin numbers come from silometer.kicad_sch/perfboard prototype.
// Kept byte-identical in embedded/hardware_verification/include/board_pins.h;
// the two copies are synced by hand.

#include <stdint.h>

// TMF8829_EVM_EB_SHIELD
static const uint8_t PIN_TOF_SDA = 21;  // shield CN4-9
static const uint8_t PIN_TOF_SCL = 22;  // shield CN4-10
static const uint8_t PIN_TOF_EN = 25;   // shield CN6-7, active high
static const uint8_t PIN_TOF_INT = 26;  // shield CN6-3, active low, open-drain

// TMF8829 datasheet section 6, bus timing: f_SCL is 400 kHz typical, 1 MHz max
// (Fast-mode Plus). The shield's translator is a TXS0104, which carries its own
// 10k pull-ups and one-shot edge acceleration - do NOT fit external 4k7 pull-ups,
// they fight the one-shot. If ranging is unreliable on long flying leads, drop
// this to 100000 instead.
static const uint32_t TOF_I2C_HZ = 400000;
static const uint8_t TOF_I2C_ADDR = 0x41;  // TMF8829 datasheet section 8.1.15

// EBYTE E32-900T20D. Net names are written from the module's point of view,
// so LORA_RXD carries ESP32 transmit data.
static const uint8_t PIN_LORA_TX = 17;   // ESP32 TX2 -> E32 pin 3 RXD
static const uint8_t PIN_LORA_RX = 16;   // ESP32 RX2 <- E32 pin 4 TXD
static const uint8_t PIN_LORA_M0 = 18;   // E32 pin 1
static const uint8_t PIN_LORA_M1 = 19;   // E32 pin 2
static const uint8_t PIN_LORA_AUX = 4;   // E32 pin 5, high = idle

// Mode 3 parameter setting always runs at 9600 8N1 regardless of the baud
// selected in SPED (E32 manual section 7), so the console baud and this one
// are not interchangeable.
static const uint32_t LORA_DEFAULT_BAUD = 9600;

// Status LED. IO2 drives the on-board LED of the DOIT DevKit V1 and is free in
// this design. It is a boot strapping pin, but the on-board LED only loads it
// after reset, so driving it as an output is safe. Override for an external LED
// with -DPIN_STATUS_LED=<gpio>; avoid IO5/IO12/IO15, which are also strapping.
#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 2
#endif

// UART0 (IO1/IO3) is deliberately left to the USB console so flashing and
// monitoring keep working while UART2 drives the radio.
static const uint32_t CONSOLE_BAUD = 115200;
