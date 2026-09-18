#pragma once

#include <stdint.h>

#include "app_config.hpp"

#ifndef ENDPOINT_ID
#error "ENDPOINT_ID not set - e.g. ENDPOINT_ID=0x0007 pio run -e endpoint_bench"
#endif

// The E32 reserves 0x0000 and 0xFFFF as broadcast/monitor addresses (manual
// sections 5.3 and 5.4): a module holding either receives every frame on its
// channel, which would defeat the per-endpoint filtering the radio gives us.
static_assert(ENDPOINT_ID != 0x0000 && ENDPOINT_ID != 0xFFFF,
              "0x0000 and 0xFFFF are reserved by the radio");
static_assert(ENDPOINT_ID != cfg::kGatewayId, "endpoint cannot take the gateway's address");

constexpr uint16_t kBuiltInEndpointId = ENDPOINT_ID;
