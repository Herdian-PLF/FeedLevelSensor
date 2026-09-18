#pragma once

#include <stdint.h>

#include "app_config.hpp"

// One aggregated measurement set. Distances are radial range in millimetres,
// with 0 meaning no target - the sensor's own sentinel, preserved rather than
// translated so the gateway sees gaps as gaps. SNR is the raw companded byte;
// it is decoded host-side, where the companding curve can change without a
// firmware release.
struct Reading {
  uint16_t distanceMm[cfg::kZoneCount];
  uint8_t snr[cfg::kZoneCount];
  uint32_t frameNumber;
  uint8_t temperatureC;
  uint8_t zones;
  uint8_t validZones;
  bool valid;
};

class TofSensor {
 public:
  bool begin();
  bool measure(uint8_t frames, Reading* out);
  void powerDown();

 private:
  bool awaitFrame(uint32_t timeoutMs);
  bool started_ = false;
};
