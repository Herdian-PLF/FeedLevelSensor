#pragma once

#include <stdint.h>

#include "persist.h"
#include "protocol.h"

// One gateway exchange, start to finish. Runs to completion and returns; there
// is no loop() residency, because every path ends in deep sleep.

enum class SessionOutcome : uint8_t {
  Acked,
  Partial,
  GatewayBusy,
  Rejected,
  NoResponse,
  Malformed,
  RadioFault,
};

const char* sessionOutcomeName(SessionOutcome outcome);

// configOut is filled only when the gateway pushed a config TLV in its ack;
// applying and persisting it is the caller's job.
SessionOutcome runSession(RtcState& state, const PersistedConfig& config, uint8_t sensorFlags,
                          proto::ConfigUpdate* configOut, bool* haveConfig);
