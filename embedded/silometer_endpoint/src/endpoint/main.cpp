// Silometer endpoint: one wake, one reading, one gateway exchange, back to sleep.
//
// Everything happens in setup(). loop() never runs, because every path through
// the cycle ends in esp_deep_sleep_start() - including the failure paths, which
// is the only property that keeps a stuck cycle from flattening the cell.

#include <Arduino.h>
#include <string.h>

#include "app_config.hpp"
#include "board_pins.h"
#include "device_id.hpp"
#include "log.hpp"
#include "measurement.h"
#include "persist.h"
#include "power.h"
#include "session.h"

namespace {

constexpr char kTag[] = "app";

TofSensor sensor;
PersistedConfig config;

uint8_t takeReading(RtcState& state) {
  uint8_t flags = 0;
  if (!sensor.begin()) {
    // Clear rather than just invalidate: a stale grid from the previous cycle
    // shipped under a fault flag would read as a real surface at the gateway.
    memset(&state.cachedReading, 0, sizeof(state.cachedReading));
    sensor.powerDown();
    return (uint8_t)(flags | proto::kFlagSensorFault);
  }
  if (!sensor.measure(config.tofFrames, &state.cachedReading)) {
    // A degraded reading still ships. An endpoint that goes silent on a sensor
    // fault is indistinguishable at the gateway from one out of range or dead,
    // and the radio energy is spent either way.
    flags |= state.cachedReading.valid ? proto::kFlagDegraded : proto::kFlagSensorFault;
  }
  sensor.powerDown();
  return flags;
}

void applyConfig(const proto::ConfigUpdate& update) {
  PersistedConfig wanted = config;
  if (update.hasReportInterval) {
    wanted.reportIntervalS = update.reportIntervalS;
    LOG_I(kTag, "gateway set the report interval to %u s", update.reportIntervalS);
  }
  if (update.hasChannel) {
    if (cfg::channelIsLegal(update.channel)) {
      wanted.channel = update.channel;
      LOG_I(kTag, "gateway set the channel to 0x%02X (%u MHz)", update.channel,
            862u + update.channel);
    } else {
      LOG_W(kTag, "gateway asked for channel 0x%02X, outside the ANATEL grants; ignored",
            update.channel);
    }
  }
  if (update.hasTofFrames && update.tofFrames > 0 && update.tofFrames <= cfg::kMaxTofFrames) {
    wanted.tofFrames = update.tofFrames;
    LOG_I(kTag, "gateway set the frame count to %u", update.tofFrames);
  }
  if (configSaveIfChanged(wanted)) {
    config = wanted;
    // A channel change only takes effect once the radio is reprogrammed.
    rtcState().radioConfigVerified = false;
  }
  if (update.reboot) {
    LOG_W(kTag, "gateway requested a reboot");
    Serial.flush();
    ESP.restart();
  }
}

uint64_t scheduleNext(RtcState& state, SessionOutcome outcome) {
  const uint64_t now = rtcNowUs();
  const bool delivered = outcome == SessionOutcome::Acked;

  if (delivered) {
    state.attempt = 0;
    ++state.seq;
    state.consecutiveFailedCycles = 0;
  } else if (outcome == SessionOutcome::GatewayBusy) {
    ++state.attempt;
    if (state.attempt < cfg::kMaxAttempts) {
      LOG_I(kTag, "busy; retrying in %lu s", (unsigned long)cfg::kBusyRetryS);
      return (uint64_t)cfg::kBusyRetryS * 1000000ULL;
    }
  } else {
    ++state.attempt;
    if (state.attempt < cfg::kMaxAttempts) {
      const uint64_t delay = cfg::retryDelayUs(&state.prngState);
      // A retry that would run past its own window is not a retry, it is the
      // next cycle arriving early.
      if (now + delay + (uint64_t)cfg::kWorstCaseCycleMs * 1000ULL <= state.windowDeadlineUs) {
        LOG_I(kTag, "attempt %u/%u; retrying in %lu s", state.attempt, cfg::kMaxAttempts,
              (unsigned long)(delay / 1000000ULL));
        return delay;
      }
      LOG_W(kTag, "no room left in this window for another attempt");
    }
  }

  if (!delivered) {
    state.attempt = 0;
    ++state.seq;
    ++state.consecutiveFailedCycles;
    state.cachedReading.valid = false;
    state.receivedMask = 0;
  }

  // Anchoring to the previous deadline rather than to now keeps retry time
  // inside the window instead of pushing every later window further out.
  state.windowDeadlineUs += cfg::cycleDelayUs(&state.prngState, config.reportIntervalS);
  if (state.windowDeadlineUs <= now + (uint64_t)cfg::kMinSleepS * 1000000ULL) {
    state.windowDeadlineUs = now + cfg::cycleDelayUs(&state.prngState, config.reportIntervalS);
  }
  return state.windowDeadlineUs - now;
}

}  // namespace

void setup() {
  powerEarlyInit();
  setCpuFrequencyMhz(cfg::kCpuMhz);
  Serial.begin(CONSOLE_BAUD);
  delay(100);

  RtcState& state = rtcState();
  const bool warm = rtcRestore();
  configLoad(&config);

  LOG_I(kTag, "silometer endpoint fw 0x%02X, profile %s, id 0x%04X, boot %lu, %s",
        cfg::kFirmwareVersion, cfg::kProfile, config.endpointId, (unsigned long)state.bootCount,
        warm ? (wokeFromTimer() ? "timer wake" : "reset") : "cold start");
  LOG_I(kTag, "reset reason %lu, rtc %lu s, %u fragment(s) of %u zones",
        (unsigned long)resetReasonCode(), (unsigned long)(rtcNowUs() / 1000000ULL),
        proto::kFragmentCount, proto::kZonesPerFragment);

  const uint32_t cycleDeadlineMs = millis() + cfg::kCycleWatchdogMs;

  if (resetWasAbnormal()) {
    // A brownout loop at 120 mA empties a cell in hours, so an abnormal reset
    // costs an attempt and backs off rather than retrying straight away.
    LOG_W(kTag, "abnormal reset; backing off without transmitting");
    state.attempt = cfg::kMaxAttempts;
    const uint64_t sleepFor = scheduleNext(state, SessionOutcome::RadioFault);
    rtcSeal();
    enterDeepSleep(sleepFor);
  }

  uint8_t sensorFlags = state.sensorFlags;
  if (state.attempt == 0 || !state.cachedReading.valid) {
    sensorFlags = takeReading(state);
    state.sensorFlags = sensorFlags;
  } else {
    LOG_I(kTag, "retry: reusing the cached reading, %u valid zone(s)",
          state.cachedReading.validZones);
  }

  SessionOutcome outcome;
  if ((int32_t)(millis() - cycleDeadlineMs) >= 0) {
    LOG_E(kTag, "cycle watchdog expired before the session started");
    outcome = SessionOutcome::RadioFault;
  } else {
    proto::ConfigUpdate update;
    bool haveConfig = false;
    outcome = runSession(state, config, sensorFlags, &update, &haveConfig);
    if (haveConfig) {
      applyConfig(update);
    }
  }

  LOG_I(kTag, "cycle %lu %s after %lu ms", (unsigned long)state.seq, sessionOutcomeName(outcome),
        (unsigned long)millis());

  const uint64_t sleepFor = scheduleNext(state, outcome);
  rtcSeal();
  enterDeepSleep(sleepFor);
}

void loop() {}
