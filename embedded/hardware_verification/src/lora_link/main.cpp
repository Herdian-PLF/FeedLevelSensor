// Two-node LoRa round-trip check for the silometer perfboard prototypes.
//
// Device A sends a ping carrying a 16-bit payload; device B logs it, returns
// payload ^ 0xFFFF, and A verifies the transformation. One firmware, two roles
// selected by -DLORA_ROLE_A / -DLORA_ROLE_B.
//
// The status LED blinks slowly while the link is healthy and fast when it is
// not, so range tests can be read off the board with no serial console attached.
// Nothing here blocks: the 4 s idle and every radio wait are driven from millis()
// so the LED keeps its cadence throughout.

#include <Arduino.h>

#include "board_pins.h"

#if defined(LORA_ROLE_A) == defined(LORA_ROLE_B)
#error "define exactly one of LORA_ROLE_A / LORA_ROLE_B"
#endif

namespace {

const uint8_t FRAME_MAGIC0 = 0xA5;
const uint8_t FRAME_MAGIC1 = 0x5A;
const uint8_t FRAME_LEN = 8;
const uint8_t TYPE_PING = 0x01;
const uint8_t TYPE_PONG = 0x02;
const uint16_t PAYLOAD_MASK = 0xFFFF;

const uint32_t CYCLE_PERIOD_MS = 4000;
const uint32_t REPLY_TIMEOUT_MS = 2000;
// The LED must show a stale link, not just the most recent exchange.
const uint32_t LINK_STALE_MS = 10000;
const uint32_t BLINK_OK_MS = 1000;    // 0.5 Hz
const uint32_t BLINK_FAIL_MS = 125;   // 4 Hz
// Gives the peer time to turn its transceiver around before the reply goes out.
const uint32_t TURNAROUND_MS = 50;
const uint32_t AUX_TIMEOUT_MS = 1000;

// ANATEL grants 902-907.5 and 915-928 MHz; carrier = 862 MHz + CHAN.
const uint8_t CHAN_915_MHZ = 0x35;
const uint8_t SPED_9600_8N1_2K4 = 0x1A;
const uint8_t OPTION_TRANSPARENT_20DBM = 0x44;

uint8_t rxBuf[FRAME_LEN];
uint8_t rxLen = 0;
uint32_t lastGoodMs = 0;
bool ledState = false;
uint32_t lastToggleMs = 0;
uint32_t okCount = 0;

#if defined(LORA_ROLE_A)
uint16_t seq = 0;
uint16_t pendingPayload = 0;
uint32_t lastCycleMs = 0;
uint32_t pingSentMs = 0;
uint32_t failCount = 0;
bool awaitingReply = false;
#endif

void setMode(uint8_t m0, uint8_t m1) {
  digitalWrite(PIN_LORA_M0, m0);
  digitalWrite(PIN_LORA_M1, m1);
  delay(5);
}

bool waitAuxHigh(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (digitalRead(PIN_LORA_AUX) == HIGH) {
      return true;
    }
  }
  return digitalRead(PIN_LORA_AUX) == HIGH;
}

uint8_t checksum(const uint8_t* frame) {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < FRAME_LEN - 1; ++i) {
    sum ^= frame[i];
  }
  return sum;
}

void sendFrame(uint8_t type, uint16_t sequence, uint16_t payload) {
  uint8_t frame[FRAME_LEN];
  frame[0] = FRAME_MAGIC0;
  frame[1] = FRAME_MAGIC1;
  frame[2] = type;
  frame[3] = (uint8_t)(sequence & 0xFF);
  frame[4] = (uint8_t)(sequence >> 8);
  frame[5] = (uint8_t)(payload & 0xFF);
  frame[6] = (uint8_t)(payload >> 8);
  frame[7] = checksum(frame);

  waitAuxHigh(AUX_TIMEOUT_MS);
  Serial2.write(frame, FRAME_LEN);
  Serial2.flush();
}

// Byte-at-a-time so a partial frame never blocks the LED or the state machine.
bool pollFrame(uint8_t* type, uint16_t* sequence, uint16_t* payload) {
  while (Serial2.available()) {
    const uint8_t b = (uint8_t)Serial2.read();
    if (rxLen == 0 && b != FRAME_MAGIC0) {
      continue;
    }
    if (rxLen == 1 && b != FRAME_MAGIC1) {
      rxLen = (b == FRAME_MAGIC0) ? 1 : 0;
      continue;
    }
    rxBuf[rxLen++] = b;
    if (rxLen < FRAME_LEN) {
      continue;
    }
    rxLen = 0;
    if (checksum(rxBuf) != rxBuf[FRAME_LEN - 1]) {
      Serial.println("rx: checksum mismatch");
      continue;
    }
    *type = rxBuf[2];
    *sequence = (uint16_t)rxBuf[3] | ((uint16_t)rxBuf[4] << 8);
    *payload = (uint16_t)rxBuf[5] | ((uint16_t)rxBuf[6] << 8);
    return true;
  }
  return false;
}

void configureRadio() {
  setMode(HIGH, HIGH);
  waitAuxHigh(AUX_TIMEOUT_MS);
  while (Serial2.available()) {
    Serial2.read();
  }

  const uint8_t read[3] = {0xC1, 0xC1, 0xC1};
  Serial2.write(read, sizeof(read));
  Serial2.flush();

  uint8_t cfg[6] = {0};
  uint8_t got = 0;
  const uint32_t deadline = millis() + AUX_TIMEOUT_MS;
  while (got < sizeof(cfg) && (int32_t)(millis() - deadline) < 0) {
    if (Serial2.available()) {
      cfg[got++] = (uint8_t)Serial2.read();
    }
  }

  if (got != sizeof(cfg) || cfg[0] != 0xC0) {
    Serial.println("WARN: could not read radio config - leaving it untouched");
  } else if (cfg[4] != CHAN_915_MHZ) {
    Serial.printf("channel 0x%02X (%u MHz) -> 0x%02X (915 MHz)\n", cfg[4], 862u + cfg[4],
                  CHAN_915_MHZ);
    const uint8_t write[6] = {0xC0,          0x00, 0x00, SPED_9600_8N1_2K4, CHAN_915_MHZ,
                              OPTION_TRANSPARENT_20DBM};
    Serial2.write(write, sizeof(write));
    Serial2.flush();
    waitAuxHigh(AUX_TIMEOUT_MS);
    delay(100);
    while (Serial2.available()) {
      Serial2.read();
    }
  } else {
    Serial.println("channel already 0x35 (915 MHz)");
  }

  setMode(LOW, LOW);
  waitAuxHigh(AUX_TIMEOUT_MS);
}

bool linkOk() { return lastGoodMs != 0 && (millis() - lastGoodMs) < LINK_STALE_MS; }

void updateLed() {
  const uint32_t interval = linkOk() ? BLINK_OK_MS : BLINK_FAIL_MS;
  if (millis() - lastToggleMs >= interval) {
    lastToggleMs = millis();
    ledState = !ledState;
    digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
  }
}

#if defined(LORA_ROLE_A)
void runRole() {
  uint8_t type;
  uint16_t rxSeq;
  uint16_t rxPayload;

  if (!awaitingReply && millis() - lastCycleMs >= CYCLE_PERIOD_MS) {
    pendingPayload = (uint16_t)(random(0, 0x10000));
    ++seq;
    Serial.printf("\n[A] seq=%u tx payload=0x%04X\n", seq, pendingPayload);
    sendFrame(TYPE_PING, seq, pendingPayload);
    pingSentMs = millis();
    awaitingReply = true;
  }

  // Always drain the UART, even outside the reply window: a pong that arrives
  // after a timeout must be consumed here or it desyncs the next cycle's parse.
  if (pollFrame(&type, &rxSeq, &rxPayload)) {
    if (awaitingReply && type == TYPE_PONG && rxSeq == seq) {
      const uint16_t expected = (uint16_t)(pendingPayload ^ PAYLOAD_MASK);
      awaitingReply = false;
      lastCycleMs = millis();
      if (rxPayload == expected) {
        ++okCount;
        lastGoodMs = millis();
        Serial.printf("[A] seq=%u rx payload=0x%04X OK (rtt %lu ms)  ok=%lu fail=%lu\n", rxSeq,
                      rxPayload, (unsigned long)(millis() - pingSentMs), (unsigned long)okCount,
                      (unsigned long)failCount);
      } else {
        ++failCount;
        Serial.printf("[A] seq=%u rx payload=0x%04X BAD, expected 0x%04X  ok=%lu fail=%lu\n", rxSeq,
                      rxPayload, expected, (unsigned long)okCount, (unsigned long)failCount);
      }
    }
  }

  if (awaitingReply && millis() - pingSentMs >= REPLY_TIMEOUT_MS) {
    ++failCount;
    awaitingReply = false;
    lastCycleMs = millis();
    Serial.printf("[A] seq=%u TIMEOUT, no reply in %lu ms  ok=%lu fail=%lu\n", seq,
                  (unsigned long)REPLY_TIMEOUT_MS, (unsigned long)okCount,
                  (unsigned long)failCount);
  }
}
#else
void runRole() {
  uint8_t type;
  uint16_t rxSeq;
  uint16_t rxPayload;

  if (pollFrame(&type, &rxSeq, &rxPayload) && type == TYPE_PING) {
    const uint16_t reply = (uint16_t)(rxPayload ^ PAYLOAD_MASK);
    ++okCount;
    lastGoodMs = millis();
    Serial.printf("[B] seq=%u rx payload=0x%04X -> tx 0x%04X  count=%lu\n", rxSeq, rxPayload, reply,
                  (unsigned long)okCount);
    delay(TURNAROUND_MS);
    sendFrame(TYPE_PONG, rxSeq, reply);
  }
}
#endif

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  pinMode(PIN_LORA_AUX, INPUT_PULLUP);
  Serial2.begin(LORA_DEFAULT_BAUD, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);
  delay(500);

#if defined(LORA_ROLE_A)
  Serial.println("\nSILOMETER LoRa link check - role A (initiator)");
#else
  Serial.println("\nSILOMETER LoRa link check - role B (responder)");
#endif
  Serial.println("LED: 0.5 Hz = link up, 4 Hz = link down");
  Serial.println("Fit the 915 MHz antenna before powering up - this app transmits.");

  configureRadio();
#if defined(LORA_ROLE_A)
  randomSeed(esp_random());
  lastCycleMs = millis() - CYCLE_PERIOD_MS;
#endif
}

void loop() {
  updateLed();
  runRole();
}
