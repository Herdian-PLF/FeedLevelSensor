// Every tunable the endpoint alone owns: schedule, timeouts and sensor cadence.
// Two complete profiles sit side by side and one build flag picks between them,
// so a constant that exists in only one profile is a compile error rather than a
// field value quietly used on the bench. What the gateway must agree on lives in
// link_config.h instead.

#pragma once

#include <esp_random.h>
#include <esp_timer.h>
#include <stdint.h>

#include "link_config.h"
#include "protocol.h"

#if defined(BENCH_TIMING) && defined(FIELD_TIMING)
#error "pick exactly one timing profile"
#endif

namespace cfg {

constexpr uint8_t kMaxAttempts = 4;
constexpr uint8_t kInnerRetries = 2;
constexpr uint8_t kMaxTofFrames = 7;

// Each reply is at most one packet behind one of ours, so two packets on air
// bound the wait. The 1000 ms margin covers the gateway's poll and UART.
constexpr uint32_t kHelloReplyTimeoutMs = 2 * kPacketAirMs + 1000;
// If the last fragment is lost, the ack only leaves after the gateway's quiet
// timer has run from the one before it.
constexpr uint32_t kDataAckTimeoutMs = kBurstQuietMs + kPacketAirMs + 1500;
constexpr uint32_t kInterFragmentGapMs = 20;

// One round in which every wait runs to its timeout. The 20 s floor is the
// value the 2.4 kbps schedule was validated with.
constexpr uint32_t kSlowSessionMs =
    kHelloReplyTimeoutMs + proto::kFragmentCount * kAuxTimeoutMs + kDataAckTimeoutMs;
constexpr uint32_t kWorstCaseCycleMs = kSlowSessionMs > 20000 ? kSlowSessionMs : 20000;

#if defined(BENCH_TIMING)
constexpr const char* kProfile = "BENCH";
// Tracks the session length so a slow air rate still gets whole cycles on the
// bench instead of every session overrunning its own window.
constexpr uint32_t kCycleMinS = kWorstCaseCycleMs / 1000 - 2;
constexpr uint32_t kCycleMaxS = kWorstCaseCycleMs / 1000 + 2;
constexpr uint32_t kRetryMinS = 2;
constexpr uint32_t kRetryMaxS = 3;
constexpr uint32_t kBusyRetryS = 2;
constexpr uint32_t kRadioRecheckCycles = 1;
constexpr uint8_t kTofFramesPerReading = 3;
constexpr uint32_t kCycleWatchdogMs = 30000;
#else
constexpr const char* kProfile = "FIELD";
constexpr uint32_t kCycleMinS = 1500;
constexpr uint32_t kCycleMaxS = 2100;
constexpr uint32_t kRetryMinS = 10;
constexpr uint32_t kRetryMaxS = 120;
constexpr uint32_t kBusyRetryS = 5;
constexpr uint32_t kRadioRecheckCycles = 24;
constexpr uint8_t kTofFramesPerReading = 5;
constexpr uint32_t kCycleWatchdogMs = 60000;
#endif

constexpr uint32_t kTofCpuReadyTimeoutMs = 100;
constexpr uint32_t kTofFrameTimeoutMs = 1500;
constexpr uint8_t kMinValidZones = 8;

constexpr uint32_t kMinSleepS = 1;
constexpr uint8_t kCpuMhz = 80;

constexpr uint8_t kFirmwareVersion = 0x01;

static_assert(kCycleMinS < kCycleMaxS, "cycle window is inverted");
static_assert(kDataAckTimeoutMs > kBurstQuietMs + kPacketAirMs,
              "the endpoint gives up before the gateway's quiet timer can ack");
static_assert(kRetryMinS <= kRetryMaxS, "retry window is inverted");
static_assert(kRetryMaxS * kMaxAttempts < kCycleMinS, "retries must fit inside one window");
static_assert(kTofFramesPerReading <= kMaxTofFrames, "raise kMaxTofFrames");
static_assert(kTofFramesPerReading % 2 == 1, "an odd frame count makes the median unambiguous");

// esp_random() only returns true random numbers while the RF subsystem is up
// (see esp_random.h), and this firmware never brings up Wi-Fi or Bluetooth. Left
// at that, identical units booting identical firmware through identical reset
// paths can draw the same jitter, collide every window, and look exactly like a
// range problem in the field. Mixing the endpoint address into the seed of a
// per-node PRNG makes two nodes sharing a sequence impossible.
inline void seedPrng(uint32_t* state, uint16_t endpointId) {
  uint32_t seed = esp_random() ^ (uint32_t)esp_timer_get_time() ^ ((uint32_t)endpointId * 2654435761UL);
  *state = seed ? seed : 0x1234567UL;  // xorshift cannot escape zero
}

inline uint32_t nextRandom(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

inline uint32_t randomRangeS(uint32_t* state, uint32_t minS, uint32_t maxS) {
  if (maxS <= minS) {
    return minS;
  }
  return minS + nextRandom(state) % (maxS - minS + 1);
}

// A gateway-pushed interval keeps the profile's relative spread rather than its
// absolute one, so a short bench interval does not inherit a +/-300 s jitter.
inline uint64_t cycleDelayUs(uint32_t* state, uint16_t overrideIntervalS) {
  uint32_t lo = kCycleMinS;
  uint32_t hi = kCycleMaxS;
  if (overrideIntervalS > 0) {
    const uint32_t spread = ((uint32_t)overrideIntervalS * (kCycleMaxS - kCycleMinS)) /
                            (kCycleMinS + kCycleMaxS);
    lo = overrideIntervalS > spread ? overrideIntervalS - spread : 1;
    hi = overrideIntervalS + spread;
  }
  return (uint64_t)randomRangeS(state, lo, hi) * 1000000ULL;
}

inline uint64_t retryDelayUs(uint32_t* state) {
  return (uint64_t)randomRangeS(state, kRetryMinS, kRetryMaxS) * 1000000ULL;
}

}  // namespace cfg
