// Gateway half of the point-to-point session specified in
// docs/protocol/lora-p2p-v0.1.md: answer HELLO, collect the fragment burst,
// acknowledge it with the received mask and carry queued configuration back down.
// Deliberately free of Arduino, the radio and the Bridge - time arrives as a
// parameter and frames leave through a buffer - so the whole state machine runs
// on a host under scripts/gateway_selftest.sh.

#pragma once

#include <stdint.h>

#include "gateway_config.h"
#include "protocol/protocol.h"

namespace gw {

struct Reading {
  uint16_t src;
  uint8_t seq;
  uint16_t mask;
  proto::HelloInfo hello;
  uint16_t distanceMm[cfg::kZoneCount];
  uint8_t snr[cfg::kZoneCount];
};

// One call can raise more than one flag: closing a burst both answers the
// endpoint and hands a reading up.
struct Action {
  bool transmit = false;
  uint16_t dest = 0;
  uint8_t len = 0;
  bool helloReceived = false;
  bool readingReady = false;
};

class Session {
 public:
  // Rejects a channel outside the ANATEL grants and an interval of zero rather
  // than relaying them: an endpoint that applies a bad channel is off the air
  // until someone walks out to it.
  bool queueConfig(uint16_t endpoint, const proto::ConfigUpdate& update);

  Action onFrame(const proto::Frame& frame, uint32_t nowMs);
  Action tick(uint32_t nowMs);

  // Valid until the next onFrame() or tick().
  const uint8_t* frame() const { return tx_; }
  const Reading& reading() const { return reading_; }

  bool burstOpen() const { return burstOpen_; }
  uint16_t peer() const { return peer_; }
  uint8_t peerSeq() const { return peerSeq_; }
  uint16_t mask() const { return mask_; }

 private:
  struct Pending {
    bool used = false;
    uint16_t endpoint = 0;
    proto::ConfigUpdate update{};
  };

  Action onHello(const proto::Frame& frame);
  Action onData(const proto::Frame& frame, uint32_t nowMs);
  uint8_t buildConfigTlv(uint16_t endpoint, uint8_t* out, uint8_t max);

  uint8_t tx_[proto::kMaxFrameBytes] = {0};
  Reading reading_{};

  uint16_t peer_ = 0;
  uint8_t peerSeq_ = 0;
  uint16_t mask_ = 0;
  bool burstOpen_ = false;
  uint32_t lastFragmentMs_ = 0;

  // The endpoint retries the missing fragments inside the same window, so the
  // same (src, seq) closes up to three times. Remembering what was last handed
  // up turns those into one forward per round that actually gained a fragment.
  bool forwarded_ = false;
  uint16_t forwardedSrc_ = 0;
  uint8_t forwardedSeq_ = 0;
  uint16_t forwardedMask_ = 0;

  Pending pending_[kMaxPendingConfigs];
};

}  // namespace gw
