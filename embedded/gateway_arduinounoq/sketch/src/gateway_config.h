// Everything the gateway alone owns: the UNO Q wiring to the E32 and the timings
// that follow from the endpoint's own retry policy. The registers, frame geometry
// and gateway address are shared with the endpoint and come from link_config.h.

#pragma once

#include <stdint.h>

#include "protocol/link_config.h"

namespace gw {

// Serial1 is usart1 on header pins D0 (RX) and D1 (TX). Serial is lpuart1, which
// the zephyr core routes to the Bridge and the Monitor, so the radio cannot have
// it. Arduino_RouterBridge issue #76 reported that opening a second UART killed
// the Bridge for the rest of the boot; the maintainer could not reproduce it on
// arduino:zephyr 1.0.0, which is why sketch.yaml pins that version.
constexpr uint8_t kLoraM0Pin = 2;
constexpr uint8_t kLoraM1Pin = 3;
constexpr uint8_t kLoraAuxPin = 4;

// Mode 3 parameter setting always runs at 9600 8N1 regardless of the baud
// selected in SPED (E32 manual section 7).
constexpr uint32_t kLoraBaud = 9600;

// The ack goes out as soon as every fragment has landed; the quiet timer is only
// the fallback for a burst that lost one. It must exceed the time one fragment
// takes end to end - ~51 ms of UART plus ~173 ms on air at 2.4 kbps plus the
// endpoint's inter-fragment gap, measured at roughly 540 ms - or the gateway acks
// mid-burst and the endpoint is still transmitting when the reply arrives.
constexpr uint32_t kBurstQuietMs = 1200;

// One poll must stay well inside the endpoint's 2000 ms HELLO reply timeout,
// since a HELLO that lands just after a poll opens waits out the rest of it.
constexpr uint32_t kPollMs = 50;

// One farm, one gateway, so an endpoint roster this size is a config table
// rather than a scaling decision.
constexpr uint8_t kMaxPendingConfigs = 8;

constexpr uint8_t kGatewayFwVersion = 0x01;

}  // namespace gw
