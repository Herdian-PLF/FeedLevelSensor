// Constants both ends of the LoRa link must agree on: the E32 registers and
// timings taken from E32-900T20D_UserManual_EN_v1.3, and the frame and zone
// geometry the wire format is derived from. Kept apart from the endpoint's
// app_config.hpp so the gateway, which shares neither the schedule nor the
// ESP-IDF headers, can compile against the same numbers instead of a copy.

#pragma once

#include <stdint.h>

namespace cfg {

// 8x8 long range preconfig. Must match TOF_ZONES in the shim.
constexpr uint8_t kZoneCount = 64;
constexpr uint8_t kZoneGrid = 8;

// E32 manual section 2.2 gives 58 bytes as the maximum single air package but
// does not say whether the three fixed-transmission routing bytes count against
// it. Measured on the bench, they do: 55 actually usable. Longer write is
// truncated rather than sub-packed, so the excess is lost with no error anywhere.
// Raising this number would put incomplete frames on the wire to fail CRC at the
// far end.
constexpr uint8_t kMaxAirPayload = 58;
constexpr uint8_t kRoutingBytes = 3;

// E32 manual section 7.5, SPED[2:0]. Range is the requirement, so this should be
// the slowest rate the module takes - but the bench modules (C3 model 0x32,
// version 0x82) refuse 0.3k and 1.2k despite the manual: the C0 echo returns the
// old SPED and the C4 readback confirms it. 4.8k is accepted, so the module
// rejects only the rates below 2.4k, and 2.4k is the longest range they offer.
// Every air-time-bound timeout on both ends derives from this, so a range sweep
// changes only kAirRate.
enum class AirRate : uint8_t {
  k0_3 = 0b000,
  k1_2 = 0b001,
  k2_4 = 0b010,
  k4_8 = 0b011,
  k9_6 = 0b100,
  k19_2 = 0b101,
};
constexpr AirRate kAirRate = AirRate::k2_4;

// SPED: [7:6] parity 00 = 8N1, [5:3] UART baud 011 = 9600, [2:0] air rate.
constexpr uint8_t kLoraSped = 0x18 | (uint8_t)kAirRate;

constexpr uint32_t airRateBps(AirRate rate) {
  return rate == AirRate::k0_3   ? 300
         : rate == AirRate::k1_2 ? 1200
         : rate == AirRate::k2_4 ? 2400
         : rate == AirRate::k4_8 ? 4800
         : rate == AirRate::k9_6 ? 9600
                                 : 19200;
}

// The manual gives no time on air. At 2.4 kbps a full fragment measured ~540 ms
// end to end, ~51 ms of it UART and 20 ms the endpoint's inter-fragment gap,
// leaving ~470 ms on air. Scaling that by rate is an extrapolation; re-measure
// with the probe's air-time burst after changing kAirRate.
constexpr uint32_t kPacketAirMsAt2k4 = 470;
constexpr uint32_t kPacketAirMs = kPacketAirMsAt2k4 * 2400 / airRateBps(kAirRate);

// OPTION: [7] 1 = fixed transmission, [6] 1 = push-pull IO, [5:3] 000 = 250 ms
// wake-up, [2] 1 = FEC on, [1:0] 00 = 20 dBm. Drop to 0xC5 for 17 dBm if the
// 120 mA transmit step browns out the unregulated cell.
constexpr uint8_t kLoraOption = 0xC4;

// Carrier = 862 MHz + CHAN. 0x35 = 915 MHz.
constexpr uint8_t kLoraChannel = 0x35;

// ANATEL grants 902-907.5 and 915-928 MHz; 862 + CHAN must land inside one.
constexpr bool channelIsLegal(uint8_t chan) {
  return (chan >= 0x28 && chan <= 0x2D) || (chan >= 0x35 && chan <= 0x42);
}

constexpr uint16_t kGatewayId = 0x0001; // ID or Address for grep

// AUX stays low while the previous packet is still on air, and sendTo waits it
// out before writing the next one.
constexpr uint32_t kAuxTimeoutMs = kPacketAirMs + 1000;

// Gateway side: how long after the last fragment a burst that lost one is
// declared over and acked. It must exceed one fragment end to end (UART, air,
// the endpoint's gap) or the gateway acks mid-burst while the endpoint is still
// transmitting. It lives here because the endpoint's DATA_ACK timeout has to
// outlast it.
constexpr uint32_t kBurstQuietMs = kPacketAirMs + 750;

// E32900T20D manual: the mode pins are only sampled while AUX is high, and a switch does not take 
// effect until that high level has lasted 2 ms. The margin over 2 ms is free.
constexpr uint32_t kAuxStableMs = 5;

// E32900T20D manual never says how quickly AUX falls once a command has been sent, so
// a stability check started immediately would happily measure the high level
// that preceded the command and conclude the module was done before it began.
constexpr uint32_t kAuxBusyGuardMs = 10;

// Sections 5.5 (power-on self-check), 5.6.4 note 4 (leaving mode 3 reloads the
// user parameters) and 7.4 (C4 reset) all hold AUX low for far longer than an
// ordinary mode switch.
constexpr uint32_t kAuxResetTimeoutMs = 3000;

constexpr uint32_t kRadioConfigTimeoutMs = 1000;

static_assert(channelIsLegal(kLoraChannel), "channel is outside the ANATEL grants");
static_assert(kZoneCount == kZoneGrid * kZoneGrid, "zone count and grid disagree");

}  // namespace cfg
