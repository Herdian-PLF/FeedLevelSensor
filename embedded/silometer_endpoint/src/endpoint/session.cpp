#include "session.h"

#include <Arduino.h>

#include "app_config.hpp"
#include "e32_radio.h"
#include "log.hpp"
#include "power.h"

namespace {

constexpr char kTag[] = "ses";

E32Radio radio;
uint8_t txBuf[proto::kMaxFrameBytes];
uint8_t rxBuf[proto::kMaxFrameBytes];

// Accepts only a frame from the gateway carrying this cycle's sequence number.
// Anything else - a neighbour's traffic that leaked through, a late reply to the
// previous attempt, a corrupted frame - is discarded without closing the window,
// so one bad frame cannot burn an attempt.
E32Rx awaitFrom(uint8_t seq, proto::Frame* out, uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint32_t remaining = timeoutMs;
  bool sawNoise = false;

  while (true) {
    const E32Rx result = radio.receive(out, rxBuf, sizeof(rxBuf), remaining);
    if (result == E32Rx::Ok) {
      if (out->src != cfg::kGatewayId) {
        LOG_W(kTag, "frame from 0x%04X, not the gateway; discarded", out->src);
        sawNoise = true;
      } else if (out->seq != seq) {
        LOG_W(kTag, "frame seq %u, expected %u; discarded", out->seq, seq);
        sawNoise = true;
      } else {
        return E32Rx::Ok;
      }
    } else if (result == E32Rx::Malformed) {
      sawNoise = true;
    }

    const uint32_t elapsed = millis() - start;
    if (elapsed >= timeoutMs) {
      return sawNoise ? E32Rx::Malformed : E32Rx::Timeout;
    }
    remaining = timeoutMs - elapsed;
  }
}

proto::HelloInfo buildHello(const RtcState& state, uint8_t sensorFlags) {
  proto::HelloInfo info{};
  info.fragCount = proto::kFragmentCount;
  info.zonesTotal = cfg::kZoneCount;
  info.zonesPerFragment = proto::kZonesPerFragment;
  info.flags = sensorFlags;
  if (state.attempt > 0) {
    info.flags |= proto::kFlagRetry;
  }
  info.temperatureC = state.cachedReading.temperatureC;
  info.validZones = state.cachedReading.validZones;
  info.frameNumberLo = (uint16_t)(state.cachedReading.frameNumber & 0xFFFF);
  info.bootCount = (uint16_t)(state.bootCount & 0xFFFF);
  info.resetReason = (uint8_t)resetReasonCode();
  info.consecutiveFailedCycles = state.consecutiveFailedCycles;
  info.fwVersion = cfg::kFirmwareVersion;
  return info;
}

SessionOutcome finish(SessionOutcome outcome) {
  radio.sleep();
  return outcome;
}

}  // namespace

const char* sessionOutcomeName(SessionOutcome outcome) {
  switch (outcome) {
    case SessionOutcome::Acked: return "acked";
    case SessionOutcome::Partial: return "partial";
    case SessionOutcome::GatewayBusy: return "gateway-busy";
    case SessionOutcome::Rejected: return "rejected";
    case SessionOutcome::NoResponse: return "no-response";
    case SessionOutcome::Malformed: return "malformed";
    case SessionOutcome::RadioFault: return "radio-fault";
  }
  return "?";
}

SessionOutcome runSession(RtcState& state, const PersistedConfig& config, uint8_t sensorFlags,
                          proto::ConfigUpdate* configOut, bool* haveConfig) {
  *haveConfig = false;
  const uint8_t seq = (uint8_t)(state.seq & 0xFF);

  if (!radio.begin()) {
    return SessionOutcome::RadioFault;
  }

  if (!state.radioConfigVerified || (state.bootCount % cfg::kRadioRecheckCycles) == 0) {
    const E32Config wanted = {config.endpointId, cfg::kLoraSped, config.channel, cfg::kLoraOption};
    if (!radio.ensureConfig(wanted)) {
      state.radioConfigVerified = false;
      return finish(SessionOutcome::RadioFault);
    }
    state.radioConfigVerified = true;
  }
  if (!radio.setMode(E32Mode::Normal)) {
    return finish(SessionOutcome::RadioFault);
  }

  const proto::HelloInfo hello = buildHello(state, sensorFlags);
  uint8_t len = proto::encodeHello(config.endpointId, seq, hello, txBuf, sizeof(txBuf));
  if (len == 0 || !radio.sendTo(cfg::kGatewayId, config.channel, txBuf, len)) {
    return finish(SessionOutcome::RadioFault);
  }
  LOG_I(kTag, "HELLO seq=%u flags=0x%02X valid=%u/%u", seq, hello.flags, hello.validZones,
        cfg::kZoneCount);

  proto::Frame frame;
  E32Rx rx = awaitFrom(seq, &frame, cfg::kHelloReplyTimeoutMs);
  if (rx != E32Rx::Ok) {
    return finish(rx == E32Rx::Malformed ? SessionOutcome::Malformed : SessionOutcome::NoResponse);
  }

  proto::HelloStatus status;
  if (!proto::parseHelloAck(frame, &status)) {
    return finish(SessionOutcome::Malformed);
  }
  if (status == proto::HelloStatus::Busy) {
    LOG_I(kTag, "gateway busy");
    return finish(SessionOutcome::GatewayBusy);
  }
  if (status != proto::HelloStatus::Ready) {
    LOG_W(kTag, "gateway rejected this endpoint (status %u)", (unsigned)status);
    return finish(SessionOutcome::Rejected);
  }

  // A retry carries on from whatever the gateway already has, so only the
  // fragments that never landed go back on air.
  uint16_t mask = state.receivedMask;
  SessionOutcome outcome = SessionOutcome::Partial;

  for (uint8_t round = 0; round <= cfg::kInnerRetries; ++round) {
    uint8_t sent = 0;
    for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
      if (mask & (1u << i)) {
        continue;
      }
      len = proto::encodeDataFragment(config.endpointId, seq, i, state.cachedReading.distanceMm,
                                      state.cachedReading.snr, cfg::kZoneCount, txBuf,
                                      sizeof(txBuf));
      if (len == 0 || !radio.sendTo(cfg::kGatewayId, config.channel, txBuf, len)) {
        state.receivedMask = mask;
        return finish(SessionOutcome::RadioFault);
      }
      ++sent;
      delay(cfg::kInterFragmentGapMs);
    }
    LOG_I(kTag, "DATA round %u: %u fragment(s) sent", round, sent);

    rx = awaitFrom(seq, &frame, cfg::kDataAckTimeoutMs);
    if (rx != E32Rx::Ok) {
      state.receivedMask = mask;
      return finish(rx == E32Rx::Malformed ? SessionOutcome::Malformed
                                           : SessionOutcome::NoResponse);
    }

    const uint8_t* tlv = nullptr;
    uint8_t tlvLen = 0;
    uint16_t acked = 0;
    if (!proto::parseDataAck(frame, &acked, &tlv, &tlvLen)) {
      state.receivedMask = mask;
      return finish(SessionOutcome::Malformed);
    }
    mask |= acked;
    LOG_I(kTag, "DATA_ACK mask=0x%04X of 0x%04X, %u config byte(s)", mask, proto::kCompleteMask,
          tlvLen);

    if (mask == proto::kCompleteMask) {
      outcome = SessionOutcome::Acked;
      if (tlvLen > 0) {
        if (proto::parseConfigTlv(tlv, tlvLen, configOut)) {
          *haveConfig = true;
        } else {
          LOG_W(kTag, "malformed config TLV; keeping the current config");
        }
      }
      break;
    }
  }

  state.receivedMask = (outcome == SessionOutcome::Acked) ? 0 : mask;
  if (outcome != SessionOutcome::Acked) {
    LOG_W(kTag, "burst incomplete, mask=0x%04X", mask);
  }
  return finish(outcome);
}
