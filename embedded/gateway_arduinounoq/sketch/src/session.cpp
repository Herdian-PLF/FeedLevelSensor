#include "session.h"

#include <string.h>

namespace gw {

bool Session::queueConfig(uint16_t endpoint, const proto::ConfigUpdate& update) {
  if (endpoint == 0x0000 || endpoint == 0xFFFF || endpoint == cfg::kGatewayId) {
    return false;
  }
  if (update.hasChannel && !cfg::channelIsLegal(update.channel)) {
    return false;
  }
  if (update.hasReportInterval && update.reportIntervalS == 0) {
    return false;
  }
  if (update.hasTofFrames && update.tofFrames == 0) {
    return false;
  }
  if (!update.hasChannel && !update.hasReportInterval && !update.hasTofFrames && !update.reboot) {
    return false;
  }

  Pending* slot = nullptr;
  for (Pending& p : pending_) {
    if (p.used && p.endpoint == endpoint) {
      slot = &p;
      break;
    }
    if (!p.used && slot == nullptr) {
      slot = &p;
    }
  }
  if (slot == nullptr) {
    return false;
  }
  slot->used = true;
  slot->endpoint = endpoint;
  slot->update = update;
  return true;
}

uint8_t Session::buildConfigTlv(uint16_t endpoint, uint8_t* out, uint8_t max) {
  for (Pending& p : pending_) {
    if (!p.used || p.endpoint != endpoint) {
      continue;
    }
    uint8_t len = 0;
    if (p.update.hasReportInterval && (uint8_t)(len + 4) <= max) {
      out[len++] = proto::kTlvReportInterval;
      out[len++] = 2;
      out[len++] = (uint8_t)(p.update.reportIntervalS & 0xFF);
      out[len++] = (uint8_t)(p.update.reportIntervalS >> 8);
    }
    if (p.update.hasChannel && (uint8_t)(len + 3) <= max) {
      out[len++] = proto::kTlvChannel;
      out[len++] = 1;
      out[len++] = p.update.channel;
    }
    if (p.update.hasTofFrames && (uint8_t)(len + 3) <= max) {
      out[len++] = proto::kTlvTofFrames;
      out[len++] = 1;
      out[len++] = p.update.tofFrames;
    }
    if (p.update.reboot && (uint8_t)(len + 2) <= max) {
      out[len++] = proto::kTlvReboot;
      out[len++] = 0;
    }
    // v0.1 carries no confirmation that a config was applied, so a DATA_ACK lost
    // on the way down loses the config with it. The MPU re-queues if it cares.
    p = Pending{};
    return len;
  }
  return 0;
}

Action Session::onHello(const proto::Frame& frame) {
  Action action;
  proto::HelloInfo info;
  if (!proto::parseHello(frame, &info)) {
    return action;
  }

  // Half duplex: mid-burst with another endpoint the gateway is genuinely deaf,
  // and saying so buys that endpoint a seconds-long retry instead of the long
  // backoff it would take from silence.
  const proto::HelloStatus status = (burstOpen_ && frame.src != peer_)
                                        ? proto::HelloStatus::Busy
                                        : proto::HelloStatus::Ready;

  action.len = proto::encodeHelloAck(cfg::kGatewayId, frame.seq, status, tx_, sizeof(tx_));
  action.transmit = action.len > 0;
  action.dest = frame.src;
  if (status != proto::HelloStatus::Ready) {
    return action;
  }

  // A new window starts from an empty grid; a retry of the same window keeps
  // what already arrived, which is what makes a lost fragment cost one
  // retransmission instead of the whole burst.
  if (frame.src != peer_ || frame.seq != peerSeq_) {
    mask_ = 0;
    memset(reading_.distanceMm, 0, sizeof(reading_.distanceMm));
    memset(reading_.snr, 0, sizeof(reading_.snr));
  }
  peer_ = frame.src;
  peerSeq_ = frame.seq;
  reading_.src = frame.src;
  reading_.seq = frame.seq;
  reading_.hello = info;
  action.helloReceived = true;

  // The burst opens on the first fragment, not here: the endpoint still has to
  // encode and transmit, which takes far longer than the quiet timeout.
  return action;
}

Action Session::onData(const proto::Frame& frame, uint32_t nowMs) {
  Action action;
  if (frame.src != peer_ || frame.seq != peerSeq_) {
    return action;
  }
  uint8_t written = 0;
  if (!proto::parseDataFragment(frame, reading_.distanceMm, reading_.snr, cfg::kZoneCount,
                                &written)) {
    return action;
  }
  mask_ |= (uint16_t)(1u << frame.fragIndex);
  lastFragmentMs_ = nowMs;
  burstOpen_ = true;
  return action;
}

Action Session::onFrame(const proto::Frame& frame, uint32_t nowMs) {
  switch (frame.type) {
    case proto::Type::Hello: return onHello(frame);
    case proto::Type::Data: return onData(frame, nowMs);
    default: return Action{};
  }
}

Action Session::tick(uint32_t nowMs) {
  Action action;
  if (!burstOpen_) {
    return action;
  }
  const bool complete = mask_ == proto::kCompleteMask;
  if (!complete && (nowMs - lastFragmentMs_) < kBurstQuietMs) {
    return action;
  }
  burstOpen_ = false;
  reading_.mask = mask_;

  uint8_t tlv[16];
  const uint8_t tlvLen = buildConfigTlv(peer_, tlv, sizeof(tlv));
  action.len = proto::encodeDataAck(cfg::kGatewayId, peerSeq_, mask_, tlv, tlvLen, tx_, sizeof(tx_));
  action.transmit = action.len > 0;
  action.dest = peer_;

  const bool sameWindow = forwarded_ && forwardedSrc_ == peer_ && forwardedSeq_ == peerSeq_;
  if (!sameWindow || forwardedMask_ != mask_) {
    action.readingReady = true;
    forwarded_ = true;
    forwardedSrc_ = peer_;
    forwardedSeq_ = peerSeq_;
    forwardedMask_ = mask_;
  }

  if (complete) {
    mask_ = 0;
  }
  return action;
}

}  // namespace gw
