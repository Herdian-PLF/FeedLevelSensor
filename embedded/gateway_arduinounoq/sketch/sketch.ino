#include <Arduino.h>

#include "Arduino_RouterBridge.h"

#include <vector>

#include "src/e32_radio.h"
#include "src/gateway_config.h"
#include "src/monitor_log.h"
#include "src/protocol/protocol.h"
#include "src/session.h"

namespace {

gw::E32Radio radio;
gw::Session session;

uint8_t rxBuf[proto::kMaxFrameBytes];
bool radioReady = false;
uint32_t lastRadioComplaintMs = 0;

// Reused rather than built per chunk: Bridge.notify packs its parameters before
// it returns, and both are only ever touched from the loop thread.
std::vector<int> distances;
std::vector<int> snr;

void forwardHello(const gw::Reading& reading) {
    const proto::HelloInfo& h = reading.hello;
    Bridge.notify("on_hello", (int)reading.src, (int)reading.seq, (int)h.flags,
                  (int)h.temperatureC, (int)h.validZones, (int)h.frameNumberLo, (int)h.bootCount,
                  (int)h.consecutiveFailedCycles, (int)h.fwVersion);
}

// One RPC carries the whole 64-zone grid at roughly 280 bytes, and RPClite's
// request buffer is DECODER_BUFFER_SIZE / 4 = 256. The grid therefore goes up in
// the same fragments the radio delivered it in - only the ones that actually
// arrived - and on_reading closes the window once they are all through.
void forwardReading(const gw::Reading& reading) {
    for (uint8_t i = 0; i < proto::kFragmentCount; ++i) {
        if ((reading.mask & (1u << i)) == 0) {
            continue;
        }
        const uint8_t first = proto::firstZoneOfFragment(i);
        const uint8_t zones = proto::zonesInFragment(i);
        distances.assign(reading.distanceMm + first, reading.distanceMm + first + zones);
        snr.assign(reading.snr + first, reading.snr + first + zones);
        Bridge.notify("on_zones", (int)reading.src, (int)reading.seq, (int)first, distances, snr);
    }
    Bridge.notify("on_reading", (int)reading.src, (int)reading.seq, (int)reading.mask,
                  (int)(reading.mask == proto::kCompleteMask));
}

void apply(const gw::Action& action) {
    // The radio reply goes out before anything reaches the MPU: the endpoint is
    // holding a 2000 ms HELLO or 3000 ms DATA_ACK window open, and the Bridge is
    // a different processor's scheduling problem.
    if (action.transmit && !radio.sendTo(action.dest, cfg::kLoraChannel, session.frame(),
                                         action.len)) {
        gw::logf("gw: reply to 0x%04X failed", action.dest);
    }
    if (action.helloReceived) {
        const gw::Reading& r = session.reading();
        gw::logf("gw: HELLO from 0x%04X seq=%u valid=%u boot=%u fails=%u flags=0x%02X",
                 r.src, r.seq, r.hello.validZones, r.hello.bootCount,
                 r.hello.consecutiveFailedCycles, r.hello.flags);
        forwardHello(r);
    }
    if (action.readingReady) {
        const gw::Reading& r = session.reading();
        gw::logf("gw: reading 0x%04X seq=%u mask=0x%04X of 0x%04X", r.src, r.seq, r.mask,
                 proto::kCompleteMask);
        forwardReading(r);
    }
}

// -1 leaves a field alone, so the MPU can push one setting without restating the
// others. Validation lives in Session::queueConfig, which is what the host
// self-test exercises.
int queue_config(int endpoint, int interval_s, int channel, int tof_frames, int reboot) {
    proto::ConfigUpdate update{};
    if (interval_s >= 0) {
        update.hasReportInterval = true;
        update.reportIntervalS = (uint16_t)interval_s;
    }
    if (channel >= 0) {
        update.hasChannel = true;
        update.channel = (uint8_t)channel;
    }
    if (tof_frames >= 0) {
        update.hasTofFrames = true;
        update.tofFrames = (uint8_t)tof_frames;
    }
    update.reboot = reboot > 0;

    const bool queued = session.queueConfig((uint16_t)endpoint, update);
    gw::logf("gw: config for 0x%04X %s", (unsigned)endpoint, queued ? "queued" : "rejected");
    return queued ? 1 : 0;
}

}  // namespace

void setup() {
    Monitor.begin(115200);
    Bridge.begin();

    // provide_safe, not provide: the handler writes the pending-config table that
    // loop() reads. provide_safe runs it on the loop thread, provide runs it on
    // the Bridge thread.
    Bridge.provide_safe("queue_config", queue_config);

    distances.reserve(proto::kZonesPerFragment);
    snr.reserve(proto::kZonesPerFragment);

    if (!radio.begin()) {
        gw::logf("gw: radio begin failed");
        return;
    }
    const gw::E32Config wanted = {cfg::kGatewayId, cfg::kLoraSped, cfg::kLoraChannel,
                                  cfg::kLoraOption};
    if (!radio.ensureConfig(wanted)) {
        gw::logf("gw: radio config failed");
        return;
    }
    if (!radio.setMode(gw::E32Mode::Normal)) {
        gw::logf("gw: radio would not enter normal mode");
        return;
    }
    radioReady = true;
    gw::logf("gw: listening as 0x%04X on channel 0x%02X (%u MHz), expecting %u fragments",
             cfg::kGatewayId, cfg::kLoraChannel, 862u + cfg::kLoraChannel, proto::kFragmentCount);
    Bridge.notify("on_gateway_up", (int)cfg::kGatewayId, (int)cfg::kLoraChannel,
                  (int)proto::kFragmentCount, (int)gw::kGatewayFwVersion);
}

void loop() {
    // A dead radio must not take the Bridge down with it: the MPU stays
    // reachable and keeps being told why no readings are arriving.
    if (!radioReady) {
        delay(gw::kPollMs);
        const uint32_t now = millis();
        if (now - lastRadioComplaintMs >= 10000) {
            lastRadioComplaintMs = now;
            Bridge.notify("on_gateway_fault", (int)cfg::kGatewayId);
        }
        return;
    }

    proto::Frame frame;
    if (radio.receive(&frame, rxBuf, sizeof(rxBuf), gw::kPollMs) == gw::E32Rx::Ok) {
        apply(session.onFrame(frame, millis()));
    }
    apply(session.tick(millis()));
}
