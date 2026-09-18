// Every tunable the endpoint has: schedule, timeouts, radio registers and
// sensor cadence. Two complete profiles sit side by side and one build flag
// picks between them, so a constant that exists in only one profile is a
// compile error rather than a field value quietly used on the bench.

#pragma once

#include <esp_random.h>
#include <esp_timer.h>
#include <stdint.h>

#if defined(BENCH_TIMING) && defined(FIELD_TIMING)
#error "pick exactly one timing profile"
#endif

namespace cfg {

#if defined(BENCH_TIMING)
constexpr const char* kProfile = "BENCH";
constexpr uint32_t kCycleMinS = 18;
constexpr uint32_t kCycleMaxS = 22;
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

constexpr uint8_t kMaxAttempts = 4;
constexpr uint8_t kInnerRetries = 2;
constexpr uint8_t kMaxTofFrames = 7;

constexpr uint32_t kHelloReplyTimeoutMs = 2000;
constexpr uint32_t kDataAckTimeoutMs = 3000;
constexpr uint32_t kInterFragmentGapMs = 20;

constexpr uint32_t kAuxTimeoutMs = 1000;
constexpr uint32_t kAuxSettleMs = 5;
constexpr uint32_t kModeSwitchSettleMs = 5;
constexpr uint32_t kRadioConfigTimeoutMs = 1000;

constexpr uint32_t kTofCpuReadyTimeoutMs = 100;
constexpr uint32_t kTofFrameTimeoutMs = 1500;
constexpr uint8_t kMinValidZones = 8;

constexpr uint32_t kMinSleepS = 1;
constexpr uint32_t kWorstCaseCycleMs = 20000;
constexpr uint8_t kCpuMhz = 80;

// 8x8 long range preconfig. Must match TOF_ZONES in the shim.
constexpr uint8_t kZoneCount = 64;
constexpr uint8_t kZoneGrid = 8;

// E32 manual section 2.2: 58 bytes is the maximum single air package, beyond
// which the module sub-packs on its own. Whether the three fixed-transmission
// routing bytes count against this is NOT stated in the manual - the value here
// assumes they do, which is the conservative reading. Phase 1 of the bench plan
// measures it; raising this number is the only change needed if they do not.
constexpr uint8_t kMaxAirPayload = 58;
constexpr uint8_t kRoutingBytes = 3;

// E32 manual section 7.5. SPED: [7:6] parity 00 = 8N1, [5:3] UART baud
// 011 = 9600, [2:0] air rate 010 = 2.4k. Swap the low three bits to sweep air
// rate in a range test: 000 = 0.3k, 001 = 1.2k, 011 = 4.8k, 100 = 9.6k.
constexpr uint8_t kLoraSped = 0x1A;

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

constexpr uint16_t kGatewayId = 0x0001;
constexpr uint8_t kFirmwareVersion = 0x01;

static_assert(kCycleMinS < kCycleMaxS, "cycle window is inverted");
static_assert(kRetryMinS <= kRetryMaxS, "retry window is inverted");
static_assert(kRetryMaxS * kMaxAttempts < kCycleMinS, "retries must fit inside one window");
static_assert(kTofFramesPerReading <= kMaxTofFrames, "raise kMaxTofFrames");
static_assert(kTofFramesPerReading % 2 == 1, "an odd frame count makes the median unambiguous");
static_assert(channelIsLegal(kLoraChannel), "channel is outside the ANATEL grants");
static_assert(kZoneCount == kZoneGrid * kZoneGrid, "zone count and grid disagree");

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
