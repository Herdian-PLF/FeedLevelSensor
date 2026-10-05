// Host-side checks for the gateway session state machine. Session is written
// free of Arduino, the radio and the Bridge precisely so the paths that are
// awkward to provoke over the air - a lost fragment, a second endpoint arriving
// mid-burst, a config push - can be exercised without a board or a radio.

#include <stdio.h>
#include <string.h>

#include "session.h"

static int failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                            \
    }                                                        \
  } while (0)

namespace {

struct Grid {
  uint16_t distanceMm[cfg::kZoneCount];
  uint8_t snr[cfg::kZoneCount];
};

Grid makeGrid(uint16_t base) {
  Grid g{};
  for (uint8_t z = 0; z < cfg::kZoneCount; ++z) {
    g.distanceMm[z] = (uint16_t)(base + z * 11);
    g.snr[z] = (uint8_t)(z * 2);
  }
  g.distanceMm[3] = 0;  // no target, the sensor's own sentinel
  return g;
}

proto::HelloInfo makeHello() {
  proto::HelloInfo h{};
  h.fragCount = proto::kFragmentCount;
  h.zonesTotal = cfg::kZoneCount;
  h.zonesPerFragment = proto::kZonesPerFragment;
  h.flags = 0;
  h.temperatureC = 31;
  h.validZones = 57;
  h.frameNumberLo = 4242;
  h.bootCount = 9;
  h.consecutiveFailedCycles = 0;
  h.fwVersion = 0x01;
  return h;
}

// One endpoint transmission, decoded the way the radio would hand it over.
struct Wire {
  uint8_t bytes[proto::kMaxFrameBytes];
  proto::Frame frame;
};

bool wireHello(Wire* w, uint16_t src, uint8_t seq, const proto::HelloInfo& info) {
  const uint8_t len = proto::encodeHello(src, seq, info, w->bytes, sizeof(w->bytes));
  return len > 0 && proto::decode(w->bytes, len, &w->frame) == proto::Decode::Ok;
}

bool wireFragment(Wire* w, uint16_t src, uint8_t seq, uint8_t index, const Grid& g) {
  const uint8_t len = proto::encodeDataFragment(src, seq, index, g.distanceMm, g.snr,
                                                cfg::kZoneCount, w->bytes, sizeof(w->bytes));
  return len > 0 && proto::decode(w->bytes, len, &w->frame) == proto::Decode::Ok;
}

proto::HelloStatus ackStatus(const gw::Session& s, const gw::Action& a) {
  proto::Frame reply;
  proto::HelloStatus status = proto::HelloStatus::Reject;
  CHECK(proto::decode(s.frame(), a.len, &reply) == proto::Decode::Ok);
  CHECK(reply.src == cfg::kGatewayId);
  CHECK(proto::parseHelloAck(reply, &status));
  return status;
}

uint16_t ackMask(const gw::Session& s, const gw::Action& a, proto::ConfigUpdate* config) {
  proto::Frame reply;
  uint16_t mask = 0;
  const uint8_t* tlv = nullptr;
  uint8_t tlvLen = 0;
  CHECK(proto::decode(s.frame(), a.len, &reply) == proto::Decode::Ok);
  CHECK(reply.type == proto::Type::DataAck);
  CHECK(reply.src == cfg::kGatewayId);
  CHECK(proto::parseDataAck(reply, &mask, &tlv, &tlvLen));
  *config = proto::ConfigUpdate{};
  CHECK(proto::parseConfigTlv(tlv, tlvLen, config));
  return mask;
}

void testHelloAndFullBurst() {
  gw::Session s;
  const Grid g = makeGrid(1000);
  const proto::HelloInfo hello = makeHello();
  uint32_t now = 1000;
  Wire w;

  CHECK(wireHello(&w, 0x0007, 42, hello));
  gw::Action a = s.onFrame(w.frame, now);
  CHECK(a.transmit);
  CHECK(a.dest == 0x0007);
  CHECK(a.helloReceived);
  CHECK(ackStatus(s, a) == proto::HelloStatus::Ready);
  CHECK(s.reading().hello.validZones == 57);
  CHECK(s.reading().hello.bootCount == 9);

  // Nothing is acknowledged before a fragment arrives.
  CHECK(!s.tick(now).transmit);

  for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
    now += 300;
    CHECK(wireFragment(&w, 0x0007, 42, i, g));
    CHECK(!s.onFrame(w.frame, now).transmit);
  }

  // A complete burst is acked immediately, without waiting out the quiet timer.
  a = s.tick(now);
  CHECK(a.transmit);
  CHECK(a.dest == 0x0007);
  CHECK(a.readingReady);
  proto::ConfigUpdate config;
  CHECK(ackMask(s, a, &config) == proto::kCompleteMask);

  const gw::Reading& r = s.reading();
  CHECK(r.src == 0x0007);
  CHECK(r.seq == 42);
  CHECK(r.mask == proto::kCompleteMask);
  CHECK(memcmp(r.distanceMm, g.distanceMm, sizeof(g.distanceMm)) == 0);
  CHECK(memcmp(r.snr, g.snr, sizeof(g.snr)) == 0);
}

void testBusyWhileAnotherBurstIsOpen() {
  gw::Session s;
  const Grid g = makeGrid(2000);
  uint32_t now = 5000;
  Wire w;

  CHECK(wireHello(&w, 0x0007, 7, makeHello()));
  gw::Action a = s.onFrame(w.frame, now);
  CHECK(ackStatus(s, a) == proto::HelloStatus::Ready);

  now += 100;
  CHECK(wireFragment(&w, 0x0007, 7, 0, g));
  s.onFrame(w.frame, now);
  CHECK(s.burstOpen());

  now += 50;
  CHECK(wireHello(&w, 0x0008, 3, makeHello()));
  a = s.onFrame(w.frame, now);
  CHECK(a.transmit);
  CHECK(a.dest == 0x0008);
  CHECK(!a.helloReceived);
  CHECK(ackStatus(s, a) == proto::HelloStatus::Busy);

  // A BUSY answer must not disturb the session already in progress.
  CHECK(s.peer() == 0x0007);
  CHECK(s.peerSeq() == 7);
  CHECK(s.burstOpen());
}

void testLostFragmentIsRetriedWithinTheWindow() {
  gw::Session s;
  const Grid g = makeGrid(3000);
  uint32_t now = 20000;
  Wire w;
  proto::ConfigUpdate config;

  CHECK(wireHello(&w, 0x0007, 9, makeHello()));
  s.onFrame(w.frame, now);

  const uint8_t missing = 2;
  for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
    if (i == missing) {
      continue;
    }
    now += 300;
    CHECK(wireFragment(&w, 0x0007, 9, i, g));
    s.onFrame(w.frame, now);
  }

  // An incomplete burst is held open until the quiet timer expires.
  CHECK(!s.tick(now).transmit);
  now += cfg::kBurstQuietMs;
  gw::Action a = s.tick(now);
  CHECK(a.transmit);
  CHECK(a.readingReady);
  const uint16_t partial = (uint16_t)(proto::kCompleteMask & ~(1u << missing));
  CHECK(ackMask(s, a, &config) == partial);
  CHECK(s.reading().mask == partial);

  // The retry re-announces the same window, so the mask must carry over and the
  // one missing fragment must be enough to complete it.
  now += 500;
  CHECK(wireHello(&w, 0x0007, 9, makeHello()));
  s.onFrame(w.frame, now);
  CHECK(s.mask() == partial);

  now += 300;
  CHECK(wireFragment(&w, 0x0007, 9, missing, g));
  s.onFrame(w.frame, now);
  a = s.tick(now);
  CHECK(a.transmit);
  CHECK(a.readingReady);
  CHECK(ackMask(s, a, &config) == proto::kCompleteMask);
  CHECK(memcmp(s.reading().distanceMm, g.distanceMm, sizeof(g.distanceMm)) == 0);
}

void testNewWindowStartsFromAnEmptyGrid() {
  gw::Session s;
  const Grid g = makeGrid(4000);
  uint32_t now = 40000;
  Wire w;

  CHECK(wireHello(&w, 0x0007, 11, makeHello()));
  s.onFrame(w.frame, now);
  now += 100;
  CHECK(wireFragment(&w, 0x0007, 11, 0, g));
  s.onFrame(w.frame, now);
  now += cfg::kBurstQuietMs;
  s.tick(now);

  now += 1000;
  CHECK(wireHello(&w, 0x0007, 12, makeHello()));
  s.onFrame(w.frame, now);
  CHECK(s.mask() == 0);
  for (uint8_t z = 0; z < cfg::kZoneCount; ++z) {
    CHECK(s.reading().distanceMm[z] == 0);
  }
}

void testARoundThatGainsNothingIsNotForwardedTwice() {
  gw::Session s;
  const Grid g = makeGrid(5000);
  uint32_t now = 60000;
  Wire w;

  CHECK(wireHello(&w, 0x0007, 13, makeHello()));
  s.onFrame(w.frame, now);
  now += 100;
  CHECK(wireFragment(&w, 0x0007, 13, 0, g));
  s.onFrame(w.frame, now);
  now += cfg::kBurstQuietMs;
  CHECK(s.tick(now).readingReady);

  // The same fragment again: the endpoint must still be acknowledged, but the
  // MPU has already been handed this exact mask.
  now += 500;
  CHECK(wireFragment(&w, 0x0007, 13, 0, g));
  s.onFrame(w.frame, now);
  now += cfg::kBurstQuietMs;
  const gw::Action a = s.tick(now);
  CHECK(a.transmit);
  CHECK(!a.readingReady);
}

void testConfigPushRidesOneDataAck() {
  gw::Session s;
  const Grid g = makeGrid(6000);
  uint32_t now = 80000;
  Wire w;
  proto::ConfigUpdate config;

  proto::ConfigUpdate update{};
  update.hasReportInterval = true;
  update.reportIntervalS = 900;
  update.hasChannel = true;
  update.channel = 0x36;
  CHECK(s.queueConfig(0x0007, update));

  CHECK(wireHello(&w, 0x0007, 21, makeHello()));
  s.onFrame(w.frame, now);
  for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
    now += 300;
    CHECK(wireFragment(&w, 0x0007, 21, i, g));
    s.onFrame(w.frame, now);
  }
  gw::Action a = s.tick(now);
  CHECK(ackMask(s, a, &config) == proto::kCompleteMask);
  CHECK(config.hasReportInterval);
  CHECK(config.reportIntervalS == 900);
  CHECK(config.hasChannel);
  CHECK(config.channel == 0x36);

  // Consumed: the next window carries no config unless one is queued again.
  now += 1000;
  CHECK(wireHello(&w, 0x0007, 22, makeHello()));
  s.onFrame(w.frame, now);
  for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
    now += 300;
    CHECK(wireFragment(&w, 0x0007, 22, i, g));
    s.onFrame(w.frame, now);
  }
  a = s.tick(now);
  CHECK(ackMask(s, a, &config) == proto::kCompleteMask);
  CHECK(!config.hasReportInterval);
  CHECK(!config.hasChannel);
}

void testConfigIsValidatedBeforeItGoesOnAir() {
  gw::Session s;
  proto::ConfigUpdate bad{};

  bad = proto::ConfigUpdate{};
  bad.hasChannel = true;
  bad.channel = 0x00;  // 862 MHz, outside both ANATEL grants
  CHECK(!s.queueConfig(0x0007, bad));
  bad.channel = 0x30;  // 910 MHz, in the gap between the grants
  CHECK(!s.queueConfig(0x0007, bad));
  bad.channel = 0x42;
  CHECK(s.queueConfig(0x0007, bad));

  bad = proto::ConfigUpdate{};
  bad.hasReportInterval = true;
  bad.reportIntervalS = 0;
  CHECK(!s.queueConfig(0x0007, bad));

  bad = proto::ConfigUpdate{};
  bad.hasTofFrames = true;
  bad.tofFrames = 0;
  CHECK(!s.queueConfig(0x0007, bad));

  proto::ConfigUpdate ok{};
  ok.reboot = true;
  CHECK(!s.queueConfig(0x0000, ok));
  CHECK(!s.queueConfig(0xFFFF, ok));
  CHECK(!s.queueConfig(cfg::kGatewayId, ok));
  CHECK(s.queueConfig(0x0009, ok));

  CHECK(!s.queueConfig(0x0009, proto::ConfigUpdate{}));
}

void testConfigTableFillsAndIsBounded() {
  gw::Session s;
  proto::ConfigUpdate update{};
  update.reboot = true;
  for (uint8_t i = 0; i < gw::kMaxPendingConfigs; ++i) {
    CHECK(s.queueConfig((uint16_t)(0x0010 + i), update));
  }
  CHECK(!s.queueConfig(0x0099, update));
  // An endpoint already holding a slot replaces it rather than needing a new one.
  CHECK(s.queueConfig(0x0010, update));
}

}  // namespace

int main() {
  printf("gateway 0x%04X, %u fragments of up to %u zones, frame <= %u B\n", cfg::kGatewayId,
         proto::kFragmentCount, proto::kZonesPerFragment, proto::kMaxFrameBytes);
  testHelloAndFullBurst();
  testBusyWhileAnotherBurstIsOpen();
  testLostFragmentIsRetriedWithinTheWindow();
  testNewWindowStartsFromAnEmptyGrid();
  testARoundThatGainsNothingIsNotForwardedTwice();
  testConfigPushRidesOneDataAck();
  testConfigIsValidatedBeforeItGoesOnAir();
  testConfigTableFillsAndIsBounded();

  if (failures != 0) {
    printf("gateway self-test: %d check(s) failed\n", failures);
    return 1;
  }
  printf("gateway self-test: all checks passed\n");
  return 0;
}
