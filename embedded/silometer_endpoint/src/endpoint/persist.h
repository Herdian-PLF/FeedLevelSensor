#pragma once

#include <stdint.h>

#include "measurement.h"

// Two stores with different jobs. RtcState lives in RTC slow memory: it survives
// deep sleep, dies on power loss, and costs nothing to write, which suits the
// per-cycle bookkeeping and the cached reading. PersistedConfig lives in NVS
// because it must survive power loss - and is written only when a value actually
// changes, because flash erase cycles are finite.

constexpr uint32_t kRtcMagic = 0x5310E7EDUL;
constexpr uint16_t kRtcVersion = 1;

struct RtcState {
  uint32_t magic;
  uint16_t version;
  uint16_t receivedMask;
  uint32_t bootCount;
  uint32_t seq;
  uint64_t windowDeadlineUs;
  uint64_t lastSeenRtcUs;
  uint32_t prngState;
  uint8_t attempt;
  uint8_t consecutiveFailedCycles;
  uint8_t sensorFlags;
  bool radioConfigVerified;
  Reading cachedReading;
  uint32_t crc;
};

struct PersistedConfig {
  uint16_t endpointId;
  uint16_t reportIntervalS;
  uint8_t channel;
  uint8_t tofFrames;
};

RtcState& rtcState();

// True when the state survived intact; false means it was cold-initialised,
// which is also what happens after a power cycle or a half-written struct.
bool rtcRestore();
void rtcSeal();

void configLoad(PersistedConfig* out);
bool configSaveIfChanged(const PersistedConfig& wanted);
