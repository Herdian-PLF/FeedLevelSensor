#pragma once

#include <stdint.h>

// Wake path, pin hold management and the deep-sleep timebase. The RTC counter,
// unlike millis(), survives deep sleep, so it is what the transmission window
// is anchored to.

// Must run before anything else touches GPIO: it drives M0/M1 to their held
// values and only then releases the holds, so the radio never sees them float.
void powerEarlyInit();

uint64_t rtcNowUs();

uint32_t resetReasonCode();
bool wokeFromTimer();
bool resetWasAbnormal();

void enterDeepSleep(uint64_t durationUs);
