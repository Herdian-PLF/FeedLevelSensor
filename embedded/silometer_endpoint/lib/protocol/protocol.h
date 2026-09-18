// Wire format for the point-to-point endpoint/gateway exchange, independent of
// both the radio and the sensor so it can be exercised on a host. The frame
// layout and message semantics are specified in docs/protocol/lora-p2p-v0.1.md;
// this header is the normative encoding.

#pragma once

#include <stdint.h>

#include "app_config.hpp"

namespace proto {

constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;
constexpr uint8_t kVersion = 0x1;

constexpr uint8_t kHeaderBytes = 8;
constexpr uint8_t kCrcBytes = 2;
constexpr uint8_t kOverheadBytes = kHeaderBytes + kCrcBytes;

constexpr uint8_t kMaxFrameBytes = cfg::kMaxAirPayload - cfg::kRoutingBytes;
constexpr uint8_t kMaxPayloadBytes = kMaxFrameBytes - kOverheadBytes;

constexpr uint8_t kZoneBytes = 3;  // uint16 LE distance in mm, then the raw SNR byte
constexpr uint8_t kZonesPerFragmentCap = kMaxPayloadBytes / kZoneBytes;
constexpr uint8_t kFragmentCount =
    (cfg::kZoneCount + kZonesPerFragmentCap - 1) / kZonesPerFragmentCap;
constexpr uint8_t kZonesPerFragment = (cfg::kZoneCount + kFragmentCount - 1) / kFragmentCount;
constexpr uint16_t kCompleteMask = (uint16_t)((1UL << kFragmentCount) - 1);

static_assert(kFragmentCount <= 16, "the FRAG nibble holds at most 16 fragments");
static_assert(kZonesPerFragment * kZoneBytes + kOverheadBytes <= kMaxFrameBytes,
              "a data fragment does not fit one air packet");
static_assert(kZonesPerFragment * kFragmentCount >= cfg::kZoneCount, "fragments do not cover the grid");

enum class Type : uint8_t {
  Hello = 0x1,
  HelloAck = 0x2,
  Data = 0x3,
  DataAck = 0x4,
};

enum class HelloStatus : uint8_t {
  Ready = 0x00,
  Busy = 0x01,
  Reject = 0x02,
};

enum class Decode : uint8_t {
  Ok,
  TooShort,
  BadMagic,
  BadVersion,
  BadLength,
  BadCrc,
};

// Sensor-condition bits carried in the HELLO, so a degraded or failed reading is
// still reported rather than showing up at the gateway as silence.
constexpr uint8_t kFlagSensorFault = 1 << 0;
constexpr uint8_t kFlagDegraded = 1 << 1;
constexpr uint8_t kFlagRetry = 1 << 2;

struct HelloInfo {
  uint8_t fragCount;
  uint8_t zonesTotal;
  uint8_t zonesPerFragment;
  uint8_t flags;
  uint8_t temperatureC;
  uint8_t validZones;
  uint16_t frameNumberLo;
  uint16_t bootCount;
  uint8_t resetReason;
  uint8_t consecutiveFailedCycles;
  uint8_t fwVersion;
};

constexpr uint8_t kHelloBytes = 13;
static_assert(kHelloBytes <= kMaxPayloadBytes, "HELLO does not fit one air packet");

struct Frame {
  Type type;
  uint16_t src;
  uint8_t seq;
  uint8_t fragIndex;
  uint8_t fragCount;
  const uint8_t* payload;
  uint8_t len;
};

struct ConfigUpdate {
  bool hasReportInterval;
  uint16_t reportIntervalS;
  bool hasChannel;
  uint8_t channel;
  bool hasTofFrames;
  uint8_t tofFrames;
  bool reboot;
};

constexpr uint8_t kTlvReportInterval = 0x01;
constexpr uint8_t kTlvChannel = 0x02;
constexpr uint8_t kTlvTofFrames = 0x03;
constexpr uint8_t kTlvReboot = 0x04;
constexpr uint8_t kTlvNop = 0xFF;

uint16_t crc16(const uint8_t* data, uint16_t len);

uint8_t encodeHello(uint16_t src, uint8_t seq, const HelloInfo& info, uint8_t* out, uint8_t max);
uint8_t encodeHelloAck(uint16_t src, uint8_t seq, HelloStatus status, uint8_t* out, uint8_t max);
uint8_t encodeDataFragment(uint16_t src, uint8_t seq, uint8_t fragIndex,
                           const uint16_t* distanceMm, const uint8_t* snr, uint8_t zoneCount,
                           uint8_t* out, uint8_t max);
uint8_t encodeDataAck(uint16_t src, uint8_t seq, uint16_t receivedMask, const uint8_t* configTlv,
                      uint8_t configLen, uint8_t* out, uint8_t max);

Decode decode(const uint8_t* in, uint8_t len, Frame* out);

bool parseHello(const Frame& frame, HelloInfo* out);
bool parseHelloAck(const Frame& frame, HelloStatus* out);
bool parseDataAck(const Frame& frame, uint16_t* receivedMask, const uint8_t** configTlv,
                  uint8_t* configLen);
bool parseDataFragment(const Frame& frame, uint16_t* distanceMm, uint8_t* snr,
                       uint8_t zoneCapacity, uint8_t* zonesWritten);
bool parseConfigTlv(const uint8_t* tlv, uint8_t len, ConfigUpdate* out);

uint8_t zonesInFragment(uint8_t fragIndex);
uint8_t firstZoneOfFragment(uint8_t fragIndex);

// Reassembles frames out of a byte stream. The E32 delivers whole packets, but
// several can land back to back in the UART FIFO, and a lost sub-package can
// leave a partial one, so resynchronisation on the magic pair is required.
class FrameReader {
 public:
  void reset();
  bool feed(uint8_t byte);
  // Valid only until the next feed() that starts a new frame.
  const uint8_t* buffer() const { return buf_; }
  uint8_t length() const { return ready_; }

 private:
  uint8_t buf_[kMaxFrameBytes];
  uint8_t len_ = 0;
  uint8_t expected_ = 0;
  uint8_t ready_ = 0;
};

}  // namespace proto
