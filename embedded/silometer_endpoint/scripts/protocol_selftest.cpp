// Host-side checks for the wire format. The protocol library has no hardware
// dependencies by design, so the encoders, the CRC, fragment geometry and the
// stream reassembler can all be exercised without a board.

#include <stdio.h>
#include <string.h>

#include "protocol.h"

static int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

static void testGeometry() {
  printf("geometry: %u fragments of up to %u zones, frame <= %u B, air budget %u B\n",
         proto::kFragmentCount, proto::kZonesPerFragment, proto::kMaxFrameBytes,
         cfg::kMaxAirPayload);
  uint16_t covered = 0;
  for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
    const uint8_t zones = proto::zonesInFragment(i);
    CHECK(zones > 0);
    CHECK(proto::firstZoneOfFragment(i) == covered);
    CHECK(zones * proto::kZoneBytes + proto::kOverheadBytes <= proto::kMaxFrameBytes);
    covered = (uint16_t)(covered + zones);
  }
  CHECK(covered == cfg::kZoneCount);
  CHECK(proto::kCompleteMask == (1u << proto::kFragmentCount) - 1);
}

static void testGridRoundTrip() {
  uint16_t dist[cfg::kZoneCount];
  uint8_t snr[cfg::kZoneCount];
  for (uint8_t z = 0; z < cfg::kZoneCount; ++z) {
    dist[z] = (uint16_t)(1000 + z * 37);
    snr[z] = (uint8_t)(z * 3);
  }
  dist[5] = 0;  // no target, the sensor's own sentinel
  snr[5] = 0;

  uint16_t gotDist[cfg::kZoneCount] = {0};
  uint8_t gotSnr[cfg::kZoneCount] = {0};
  uint16_t mask = 0;

  for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
    uint8_t buf[proto::kMaxFrameBytes];
    const uint8_t len =
        proto::encodeDataFragment(0x0007, 42, i, dist, snr, cfg::kZoneCount, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(cfg::kRoutingBytes + len <= cfg::kMaxAirPayload);

    proto::Frame frame;
    CHECK(proto::decode(buf, len, &frame) == proto::Decode::Ok);
    CHECK(frame.type == proto::Type::Data);
    CHECK(frame.src == 0x0007);
    CHECK(frame.seq == 42);
    CHECK(frame.fragIndex == i);
    CHECK(frame.fragCount == proto::kFragmentCount);

    uint8_t written = 0;
    CHECK(proto::parseDataFragment(frame, gotDist, gotSnr, cfg::kZoneCount, &written));
    CHECK(written == proto::zonesInFragment(i));
    mask |= (uint16_t)(1u << i);
  }
  CHECK(mask == proto::kCompleteMask);
  CHECK(memcmp(dist, gotDist, sizeof(dist)) == 0);
  CHECK(memcmp(snr, gotSnr, sizeof(snr)) == 0);
}

static void testHello() {
  proto::HelloInfo in{};
  in.fragCount = proto::kFragmentCount;
  in.zonesTotal = cfg::kZoneCount;
  in.zonesPerFragment = proto::kZonesPerFragment;
  in.flags = proto::kFlagDegraded | proto::kFlagRetry;
  in.temperatureC = 41;
  in.validZones = 57;
  in.frameNumberLo = 0xBEEF;
  in.bootCount = 1234;
  in.resetReason = 5;
  in.consecutiveFailedCycles = 2;
  in.fwVersion = cfg::kFirmwareVersion;

  uint8_t buf[proto::kMaxFrameBytes];
  const uint8_t len = proto::encodeHello(0x0007, 9, in, buf, sizeof(buf));
  CHECK(len > 0);
  proto::Frame frame;
  CHECK(proto::decode(buf, len, &frame) == proto::Decode::Ok);
  proto::HelloInfo out{};
  CHECK(proto::parseHello(frame, &out));
  CHECK(memcmp(&in, &out, sizeof(in)) == 0);
}

static void testDataAckAndTlv() {
  const uint8_t tlv[] = {proto::kTlvReportInterval, 2,    0x2C, 0x01,
                         proto::kTlvChannel,        1,    0x36,
                         proto::kTlvNop,
                         0x77,                      1,    0xAA,  // unknown tag, must be skipped
                         proto::kTlvTofFrames,      1,    5};
  uint8_t buf[proto::kMaxFrameBytes];
  const uint8_t len =
      proto::encodeDataAck(cfg::kGatewayId, 42, 0x001B, tlv, sizeof(tlv), buf, sizeof(buf));
  CHECK(len > 0);

  proto::Frame frame;
  CHECK(proto::decode(buf, len, &frame) == proto::Decode::Ok);
  uint16_t mask = 0;
  const uint8_t* gotTlv = nullptr;
  uint8_t gotLen = 0;
  CHECK(proto::parseDataAck(frame, &mask, &gotTlv, &gotLen));
  CHECK(mask == 0x001B);
  CHECK(gotLen == sizeof(tlv));

  proto::ConfigUpdate update;
  CHECK(proto::parseConfigTlv(gotTlv, gotLen, &update));
  CHECK(update.hasReportInterval && update.reportIntervalS == 300);
  CHECK(update.hasChannel && update.channel == 0x36);
  CHECK(update.hasTofFrames && update.tofFrames == 5);
  CHECK(!update.reboot);

  const uint8_t truncated[] = {proto::kTlvReportInterval, 2, 0x2C};
  proto::ConfigUpdate bad;
  CHECK(!proto::parseConfigTlv(truncated, sizeof(truncated), &bad));
}

static void testCorruption() {
  uint8_t buf[proto::kMaxFrameBytes];
  uint8_t len = proto::encodeHelloAck(cfg::kGatewayId, 7, proto::HelloStatus::Ready, buf,
                                      sizeof(buf));
  CHECK(len > 0);
  proto::Frame frame;
  CHECK(proto::decode(buf, len, &frame) == proto::Decode::Ok);

  uint8_t bad[proto::kMaxFrameBytes];
  memcpy(bad, buf, len);
  bad[0] = 0x00;
  CHECK(proto::decode(bad, len, &frame) == proto::Decode::BadMagic);

  memcpy(bad, buf, len);
  bad[proto::kHeaderBytes] ^= 0xFF;
  CHECK(proto::decode(bad, len, &frame) == proto::Decode::BadCrc);

  memcpy(bad, buf, len);
  bad[7] = 200;
  CHECK(proto::decode(bad, len, &frame) == proto::Decode::BadLength);

  memcpy(bad, buf, len);
  bad[2] = (uint8_t)(0x90 | (uint8_t)proto::Type::HelloAck);
  CHECK(proto::decode(bad, len, &frame) == proto::Decode::BadVersion);

  CHECK(proto::decode(buf, 3, &frame) == proto::Decode::TooShort);
}

static void testReader() {
  uint8_t a[proto::kMaxFrameBytes];
  uint8_t b[proto::kMaxFrameBytes];
  const uint8_t la = proto::encodeHelloAck(cfg::kGatewayId, 1, proto::HelloStatus::Busy, a,
                                           sizeof(a));
  const uint8_t lb = proto::encodeHelloAck(cfg::kGatewayId, 2, proto::HelloStatus::Ready, b,
                                           sizeof(b));

  // Leading junk, a doubled magic byte, then two frames back to back: the shapes
  // a sub-packed transmission and a shared channel actually produce.
  uint8_t stream[256];
  uint16_t n = 0;
  stream[n++] = 0x11;
  stream[n++] = proto::kMagic0;
  stream[n++] = proto::kMagic0;
  memcpy(stream + n, a + 1, la - 1);
  n = (uint16_t)(n + la - 1);
  memcpy(stream + n, b, lb);
  n = (uint16_t)(n + lb);

  proto::FrameReader reader;
  int seen = 0;
  uint8_t seqs[4] = {0};
  for (uint16_t i = 0; i < n; ++i) {
    if (reader.feed(stream[i])) {
      proto::Frame frame;
      CHECK(proto::decode(reader.buffer(), reader.length(), &frame) == proto::Decode::Ok);
      if (seen < 4) {
        seqs[seen] = frame.seq;
      }
      ++seen;
    }
  }
  CHECK(seen == 2);
  CHECK(seqs[0] == 1);
  CHECK(seqs[1] == 2);
}

static void testJitter() {
  uint32_t a = 0;
  uint32_t b = 0;
  cfg::seedPrng(&a, 0x0007);
  cfg::seedPrng(&b, 0x0008);
  CHECK(a != b);

  uint32_t state = a;
  uint32_t lo = 0xFFFFFFFF;
  uint32_t hi = 0;
  for (int i = 0; i < 2000; ++i) {
    const uint64_t us = cfg::cycleDelayUs(&state, 0);
    const uint32_t s = (uint32_t)(us / 1000000ULL);
    if (s < lo) lo = s;
    if (s > hi) hi = s;
  }
  CHECK(lo >= cfg::kCycleMinS);
  CHECK(hi <= cfg::kCycleMaxS);
  printf("jitter over 2000 draws: %u..%u s (profile %s bounds %u..%u)\n", lo, hi, cfg::kProfile,
         cfg::kCycleMinS, cfg::kCycleMaxS);

  // Two nodes must never walk the same sequence, or the jitter buys nothing.
  uint32_t sa = 0;
  uint32_t sb = 0;
  cfg::seedPrng(&sa, 0x0007);
  cfg::seedPrng(&sb, 0x0008);
  int identical = 0;
  for (int i = 0; i < 100; ++i) {
    if (cfg::randomRangeS(&sa, 0, 1000) == cfg::randomRangeS(&sb, 0, 1000)) {
      ++identical;
    }
  }
  CHECK(identical < 20);
}

int main() {
  testGeometry();
  testGridRoundTrip();
  testHello();
  testDataAckAndTlv();
  testCorruption();
  testReader();
  testJitter();
  if (failures == 0) {
    printf("protocol self-test: all checks passed\n");
    return 0;
  }
  printf("protocol self-test: %d check(s) FAILED\n", failures);
  return 1;
}
