// Bench fixture, not the product gateway. It answers one endpoint so the
// session state machine can be exercised end to end, and offers console
// commands to force the failure paths that are otherwise hard to reach: a busy
// gateway, a rejected endpoint, a dropped fragment, a pushed config.
//
// No internet uplink, no persistence, no multi-endpoint scheduling.

#include <Arduino.h>

#include "app_config.hpp"
#include "board_pins.h"
#include "e32_radio.h"
#include "log.hpp"
#include "protocol.h"

namespace {

constexpr char kTag[] = "gw";

// The ack goes out as soon as every fragment has landed; the quiet timer is only
// the fallback for a burst that lost one. It must exceed the time one fragment
// takes end to end - ~51 ms of UART plus ~173 ms on air at 2.4 kbps plus the
// endpoint's inter-fragment gap, measured at roughly 540 ms - or the gateway acks
// mid-burst and the endpoint is still transmitting when the reply arrives.
constexpr uint32_t kBurstQuietMs = 1200;
constexpr uint32_t kPollMs = 50;

E32Radio radio;
uint8_t txBuf[proto::kMaxFrameBytes];
uint8_t rxBuf[proto::kMaxFrameBytes];

uint16_t peer = 0;
uint8_t peerSeq = 0;
uint16_t mask = 0;
uint32_t lastFragmentMs = 0;
bool burstOpen = false;

uint16_t grid[cfg::kZoneCount];
uint8_t gridSnr[cfg::kZoneCount];
proto::HelloInfo lastHello;
bool haveGrid = false;

proto::HelloStatus forcedStatus = proto::HelloStatus::Ready;
int8_t dropFragment = -1;
uint16_t pushIntervalS = 0;
bool pushReboot = false;

void printGrid() {
  if (!haveGrid) {
    Serial.println("no complete grid yet");
    return;
  }
  Serial.printf("\ngrid from 0x%04X  seq=%u  T=%uC  valid=%u/%u  flags=0x%02X\n", peer, peerSeq,
                lastHello.temperatureC, lastHello.validZones, lastHello.zonesTotal,
                lastHello.flags);
  for (uint8_t row = 0; row < cfg::kZoneGrid; ++row) {
    for (uint8_t col = 0; col < cfg::kZoneGrid; ++col) {
      const uint8_t i = (uint8_t)(row * cfg::kZoneGrid + col);
      Serial.printf(" %5u/%-3u", grid[i], gridSnr[i]);
    }
    Serial.println();
  }
}

void sendHelloAck(uint16_t dst, uint8_t seq, proto::HelloStatus status) {
  const uint8_t len = proto::encodeHelloAck(cfg::kGatewayId, seq, status, txBuf, sizeof(txBuf));
  if (len == 0 || !radio.sendTo(dst, cfg::kLoraChannel, txBuf, len)) {
    LOG_E(kTag, "HELLO_ACK send failed");
    return;
  }
  LOG_I(kTag, "HELLO_ACK -> 0x%04X seq=%u status=%u", dst, seq, (unsigned)status);
}

void sendDataAck(uint16_t dst, uint8_t seq, uint16_t acked) {
  uint8_t tlv[8];
  uint8_t tlvLen = 0;
  if (pushIntervalS > 0) {
    tlv[tlvLen++] = proto::kTlvReportInterval;
    tlv[tlvLen++] = 2;
    tlv[tlvLen++] = (uint8_t)(pushIntervalS & 0xFF);
    tlv[tlvLen++] = (uint8_t)(pushIntervalS >> 8);
    pushIntervalS = 0;
  }
  if (pushReboot) {
    tlv[tlvLen++] = proto::kTlvReboot;
    tlv[tlvLen++] = 0;
    pushReboot = false;
  }
  const uint8_t len =
      proto::encodeDataAck(cfg::kGatewayId, seq, acked, tlv, tlvLen, txBuf, sizeof(txBuf));
  if (len == 0 || !radio.sendTo(dst, cfg::kLoraChannel, txBuf, len)) {
    LOG_E(kTag, "DATA_ACK send failed");
    return;
  }
  LOG_I(kTag, "DATA_ACK -> 0x%04X seq=%u mask=0x%04X of 0x%04X, %u config byte(s)", dst, seq, acked,
        proto::kCompleteMask, tlvLen);
}

void onHello(const proto::Frame& frame) {
  proto::HelloInfo info;
  if (!proto::parseHello(frame, &info)) {
    LOG_W(kTag, "malformed HELLO");
    return;
  }
  LOG_I(kTag, "HELLO from 0x%04X seq=%u frags=%u valid=%u boot=%u reset=%u fails=%u flags=0x%02X",
        frame.src, frame.seq, info.fragCount, info.validZones, info.bootCount, info.resetReason,
        info.consecutiveFailedCycles, info.flags);

  const proto::HelloStatus status = forcedStatus;
  forcedStatus = proto::HelloStatus::Ready;
  sendHelloAck(frame.src, frame.seq, status);
  if (status != proto::HelloStatus::Ready) {
    return;
  }

  if (frame.src != peer || frame.seq != peerSeq) {
    mask = 0;
    haveGrid = false;
  }
  peer = frame.src;
  peerSeq = frame.seq;
  lastHello = info;
  // The burst opens on the first fragment, not here: the endpoint still has to
  // encode and transmit, which takes far longer than the quiet timeout.
}

void onData(const proto::Frame& frame) {
  if (frame.src != peer || frame.seq != peerSeq) {
    LOG_W(kTag, "DATA from 0x%04X seq=%u outside the open burst; ignored", frame.src, frame.seq);
    return;
  }
  if (dropFragment >= 0 && frame.fragIndex == (uint8_t)dropFragment) {
    LOG_W(kTag, "dropping fragment %u on purpose", frame.fragIndex);
    dropFragment = -1;
    lastFragmentMs = millis();
    burstOpen = true;
    return;
  }
  uint8_t written = 0;
  if (!proto::parseDataFragment(frame, grid, gridSnr, cfg::kZoneCount, &written)) {
    LOG_W(kTag, "malformed DATA fragment %u", frame.fragIndex);
    return;
  }
  mask |= (uint16_t)(1u << frame.fragIndex);
  lastFragmentMs = millis();
  burstOpen = true;
  LOG_D(kTag, "fragment %u (%u zones), mask=0x%04X", frame.fragIndex, written, mask);
}

void handleConsole() {
  if (!Serial.available()) {
    return;
  }
  switch (Serial.read()) {
    case 'b':
      forcedStatus = proto::HelloStatus::Busy;
      Serial.println("next HELLO gets BUSY");
      break;
    case 'r':
      forcedStatus = proto::HelloStatus::Reject;
      Serial.println("next HELLO gets REJECT");
      break;
    case 'd':
      dropFragment = 2;
      Serial.println("dropping fragment 2 of the next burst");
      break;
    case 'i':
      pushIntervalS = 45;
      Serial.println("pushing a 45 s report interval with the next DATA_ACK");
      break;
    case 'x':
      pushReboot = true;
      Serial.println("pushing a reboot with the next DATA_ACK");
      break;
    case 'p':
      printGrid();
      break;
    default:
      Serial.println("b=busy r=reject d=drop-frag i=push-interval x=push-reboot p=print-grid");
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(300);
  Serial.println("\nSILOMETER bench gateway - test fixture, not the product gateway");
  Serial.println("Fit the 915 MHz antenna before powering up: this transmits.");
  Serial.println("keys: b=busy r=reject d=drop-frag i=push-interval x=push-reboot p=print-grid");

  if (!radio.begin()) {
    Serial.println("radio begin failed");
    return;
  }
  const E32Config wanted = {cfg::kGatewayId, cfg::kLoraSped, cfg::kLoraChannel, cfg::kLoraOption};
  if (!radio.ensureConfig(wanted)) {
    Serial.println("radio config failed");
    return;
  }
  radio.setMode(E32Mode::Normal);
  LOG_I(kTag, "listening as 0x%04X on channel 0x%02X (%u MHz), expecting %u fragments",
        cfg::kGatewayId, cfg::kLoraChannel, 862u + cfg::kLoraChannel, proto::kFragmentCount);
}

void loop() {
  handleConsole();

  proto::Frame frame;
  if (radio.receive(&frame, rxBuf, sizeof(rxBuf), kPollMs) == E32Rx::Ok) {
    switch (frame.type) {
      case proto::Type::Hello: onHello(frame); break;
      case proto::Type::Data: onData(frame); break;
      default: LOG_W(kTag, "unexpected type %u from 0x%04X", (unsigned)frame.type, frame.src); break;
    }
  }

  const bool complete = burstOpen && mask == proto::kCompleteMask;
  if (burstOpen && (complete || (millis() - lastFragmentMs) >= kBurstQuietMs)) {
    burstOpen = false;
    if (mask == proto::kCompleteMask) {
      haveGrid = true;
    }
    sendDataAck(peer, peerSeq, mask);
    if (haveGrid) {
      printGrid();
      mask = 0;
    }
  }
}
