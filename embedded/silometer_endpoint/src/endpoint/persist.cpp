#include "persist.h"

#include <Preferences.h>
#include <stddef.h>
#include <string.h>

#include "app_config.hpp"
#include "device_id.hpp"
#include "log.hpp"
#include "power.h"
#include "protocol.h"

namespace {

constexpr char kTag[] = "nvs";
constexpr char kNamespace[] = "silometer";

RTC_DATA_ATTR RtcState g_state;

static_assert(sizeof(RtcState) < 2048, "RTC slow memory is shared with the bootloader");

// offsetof, not sizeof minus the field: RtcState is 8-byte aligned because of
// the uint64_t timestamps, so the compiler can pad after the trailing crc and
// the two are not the same number.
uint32_t stateCrc(const RtcState& s) {
  return proto::crc16((const uint8_t*)&s, (uint16_t)offsetof(RtcState, crc));
}

void coldInit() {
  memset(&g_state, 0, sizeof(g_state));
  g_state.magic = kRtcMagic;
  g_state.version = kRtcVersion;
  g_state.windowDeadlineUs = rtcNowUs();
  cfg::seedPrng(&g_state.prngState, kBuiltInEndpointId);
}

}  // namespace

RtcState& rtcState() { return g_state; }

bool rtcRestore() {
  const bool intact = g_state.magic == kRtcMagic && g_state.version == kRtcVersion &&
                      g_state.crc == stateCrc(g_state);
  if (!intact) {
    LOG_W(kTag, "RTC state cold-initialised");
    coldInit();
    g_state.bootCount = 1;
    return false;
  }
  // A counter that ran backwards means the RTC domain was reset under us, so the
  // stored deadline is on a timebase that no longer exists.
  if (rtcNowUs() < g_state.lastSeenRtcUs) {
    LOG_W(kTag, "RTC counter went backwards; re-anchoring the window");
    g_state.windowDeadlineUs = rtcNowUs();
  }
  ++g_state.bootCount;
  return true;
}

void rtcSeal() {
  g_state.lastSeenRtcUs = rtcNowUs();
  g_state.magic = kRtcMagic;
  g_state.version = kRtcVersion;
  g_state.crc = stateCrc(g_state);
}

void configLoad(PersistedConfig* out) {
  Preferences prefs;
  out->endpointId = kBuiltInEndpointId;
  out->reportIntervalS = 0;  // 0 means "use the compiled profile"
  out->channel = cfg::kLoraChannel;
  out->tofFrames = cfg::kTofFramesPerReading;

  if (!prefs.begin(kNamespace, true)) {
    // A namespace that has never been written does not exist, and Preferences
    // logs that at ERROR on every boot - noise that would camouflage a real NVS
    // failure later. Create it once, empty, so the read-only open succeeds from
    // the next wake on. Costs one flash write in the device's lifetime.
    if (prefs.begin(kNamespace, false)) {
      prefs.end();
      LOG_I(kTag, "created the NVS namespace");
    } else {
      LOG_E(kTag, "NVS unavailable; running on compiled defaults");
    }
    return;
  }
  out->endpointId = prefs.getUShort("ep_id", out->endpointId);
  out->reportIntervalS = prefs.getUShort("interval", out->reportIntervalS);
  out->channel = prefs.getUChar("chan", out->channel);
  out->tofFrames = prefs.getUChar("frames", out->tofFrames);
  prefs.end();

  if (out->endpointId == 0x0000 || out->endpointId == 0xFFFF ||
      out->endpointId == cfg::kGatewayId) {
    LOG_W(kTag, "stored endpoint id 0x%04X is reserved; using the built-in 0x%04X", out->endpointId,
          kBuiltInEndpointId);
    out->endpointId = kBuiltInEndpointId;
  }
  if (!cfg::channelIsLegal(out->channel)) {
    LOG_W(kTag, "stored channel 0x%02X is outside the ANATEL grants; using 0x%02X", out->channel,
          cfg::kLoraChannel);
    out->channel = cfg::kLoraChannel;
  }
  if (out->tofFrames == 0 || out->tofFrames > cfg::kMaxTofFrames) {
    out->tofFrames = cfg::kTofFramesPerReading;
  }
}

bool configSaveIfChanged(const PersistedConfig& wanted) {
  PersistedConfig current;
  configLoad(&current);
  if (current.endpointId == wanted.endpointId && current.reportIntervalS == wanted.reportIntervalS &&
      current.channel == wanted.channel && current.tofFrames == wanted.tofFrames) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    LOG_E(kTag, "could not open NVS for writing");
    return false;
  }
  prefs.putUShort("ep_id", wanted.endpointId);
  prefs.putUShort("interval", wanted.reportIntervalS);
  prefs.putUChar("chan", wanted.channel);
  prefs.putUChar("frames", wanted.tofFrames);
  prefs.end();
  LOG_I(kTag, "config persisted");
  return true;
}
