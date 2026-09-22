#include "Arduino_RouterBridge.h"

namespace {

constexpr uint16_t kEndpointId = 0x0007;
constexpr uint32_t kMinIntervalMs = 200;
constexpr uint32_t kMaxIntervalMs = 60000;

uint32_t report_interval_ms = 2000;
uint32_t seq = 0;
uint32_t last_report_ms = 0;

}  // namespace

uint32_t set_report_interval(uint32_t ms) {
    report_interval_ms = constrain(ms, kMinIntervalMs, kMaxIntervalMs);
    Monitor.print("report interval set to ");
    Monitor.println(report_interval_ms);
    return report_interval_ms;
}

void setup() {
    Monitor.begin(115200);
    Bridge.begin();

    // provide_safe, not provide: the handler writes report_interval_ms, which loop() reads.
    // provide_safe runs it on the loop thread, provide runs it on the Bridge thread, therefore
    // avoiding race conditions.
    Bridge.provide_safe("set_report_interval", set_report_interval);
}

void loop() {
    const uint32_t now = millis();
    if (now - last_report_ms < report_interval_ms) {
        return;
    }
    last_report_ms = now;
    seq++;

    // Stand-in for a decoded LoRa frame until the E32 driver lands.
    const uint16_t distance_mm = 1200 + (seq % 7) * 25;

    Bridge.notify("on_sample", kEndpointId, seq, distance_mm);
}
