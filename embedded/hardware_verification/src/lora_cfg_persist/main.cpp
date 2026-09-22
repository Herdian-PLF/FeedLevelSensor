// E32-900T20D parameter persistence check for the silometer perfboard prototype.
//
// Check whether a C0 write survives loss of power, or only appears to take while the module stays up.
// Boot only reads and prints; 'w' shifts the address by one and writes it back.
// Reading is kept off the boot path because a power cycle drops the USB port
// with it, so anything printed at power-up is lost before a monitor can attach -
// and a read that costs nothing can then be repeated until it is seen.
//
// Talks to the module directly rather than through lib/e32, whose write path is
// what is under suspicion. The module is only ever put into Mode 3
// (sleep/config), which emits no RF - safe to run without an antenna.

#include <Arduino.h>
#include <string.h>

#include "board_pins.h"

namespace {

const uint8_t CMD_READ_CONFIG = 0xC1;
const uint8_t CMD_WRITE_SAVED = 0xC0;
const uint8_t RESP_CONFIG_LEN = 6;

// The module holds AUX low through its power-on self-check and silently drops
// anything sent inside that window, so readiness has to be a sustained high
// rather than one lucky sample.
const uint32_t AUX_READY_STABLE_MS = 50;
const uint32_t AUX_READY_TIMEOUT_MS = 5000;

const uint32_t RESPONSE_TIMEOUT_MS = 1000;

// Deliberately far longer than the 100 ms lib/e32 allows: if a C0 commit needs
// more than that, this run has to succeed where the driver fails.
const uint32_t EEPROM_COMMIT_MS = 1000;

void printFrame(const char* label, const uint8_t* buf, uint8_t len) {
  Serial.printf("  %-12s", label);
  if (len == 0) {
    Serial.println("(no answer)");
    return;
  }
  for (uint8_t i = 0; i < len; ++i) {
    Serial.printf("%02X ", buf[i]);
  }
  if (len == RESP_CONFIG_LEN) {
    Serial.printf("   addr=0x%02X%02X sped=0x%02X chan=0x%02X opt=0x%02X", buf[1], buf[2], buf[3],
                  buf[4], buf[5]);
  }
  Serial.println();
}

bool waitModuleReady(uint32_t timeoutMs, uint32_t* settledAfterMs) {
  const uint32_t start = millis();
  uint32_t highSince = 0;
  while (millis() - start < timeoutMs) {
    if (digitalRead(PIN_LORA_AUX) == HIGH) {
      if (highSince == 0) {
        highSince = millis();
      }
      if (millis() - highSince >= AUX_READY_STABLE_MS) {
        *settledAfterMs = millis() - start;
        return true;
      }
    } else {
      highSince = 0;
    }
    delay(10);
  }
  return false;
}

void drain() {
  while (Serial2.available()) {
    Serial2.read();
  }
}

uint8_t readAnswer(uint8_t* buf, uint8_t expected, uint32_t timeoutMs) {
  uint8_t got = 0;
  const uint32_t deadline = millis() + timeoutMs;
  while (got < expected && (int32_t)(millis() - deadline) < 0) {
    if (Serial2.available()) {
      buf[got++] = (uint8_t)Serial2.read();
    }
  }
  return got;
}

uint8_t readConfig(uint8_t* buf) {
  drain();
  const uint8_t cmd[3] = {CMD_READ_CONFIG, CMD_READ_CONFIG, CMD_READ_CONFIG};
  Serial2.write(cmd, sizeof(cmd));
  Serial2.flush();
  return readAnswer(buf, RESP_CONFIG_LEN, RESPONSE_TIMEOUT_MS);
}

// 0x0000 and 0xFFFF make the module receive every frame on its channel, so the
// shift steps over them rather than parking the radio on a broadcast address.
uint16_t shiftAddress(uint16_t address) {
  const uint16_t next = (uint16_t)(address + 1);
  return (next == 0x0000 || next == 0xFFFF) ? 0x0001 : next;
}

uint8_t g_bootConfig[RESP_CONFIG_LEN];
bool g_haveBootConfig = false;

void shiftAndWrite() {
  const uint16_t oldAddress = (uint16_t)((uint16_t)g_bootConfig[1] << 8 | g_bootConfig[2]);
  const uint16_t newAddress = shiftAddress(oldAddress);
  const uint8_t command[RESP_CONFIG_LEN] = {CMD_WRITE_SAVED,
                                            (uint8_t)(newAddress >> 8),
                                            (uint8_t)(newAddress & 0xFF),
                                            g_bootConfig[3],
                                            g_bootConfig[4],
                                            g_bootConfig[5]};
  Serial.println();
  printFrame("writing", command, sizeof(command));

  drain();
  Serial2.write(command, sizeof(command));
  Serial2.flush();

  uint8_t echo[RESP_CONFIG_LEN] = {0};
  const uint8_t echoGot = readAnswer(echo, RESP_CONFIG_LEN, RESPONSE_TIMEOUT_MS);
  printFrame("write echo", echo, echoGot);

  uint32_t settledMs = 0;
  if (!waitModuleReady(AUX_READY_TIMEOUT_MS, &settledMs)) {
    Serial.println("\n  ABORT: AUX never settled high after the write");
    return;
  }
  delay(EEPROM_COMMIT_MS);

  uint8_t back[RESP_CONFIG_LEN] = {0};
  const uint8_t backGot = readConfig(back);
  printFrame("readback", back, backGot);

  const bool tookInSession =
      backGot == RESP_CONFIG_LEN && back[1] == command[1] && back[2] == command[2];
  Serial.printf("\n  in-session write: %s\n", tookInSession ? "TOOK" : "DID NOT TAKE");
  Serial.println("\n  --- now hard power cycle the board, then read again ---");
  Serial.printf("  reads addr=0x%04X -> C0 reached EEPROM\n", newAddress);
  Serial.printf("  reads anything else -> C0 only changed the live registers\n");
  memcpy(g_bootConfig, back, RESP_CONFIG_LEN);
}

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);

  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  pinMode(PIN_LORA_AUX, INPUT_PULLUP);

  digitalWrite(PIN_LORA_M0, HIGH);
  digitalWrite(PIN_LORA_M1, HIGH);

  // Waits for M0/1 signals to take effect
  delay(300);
  Serial.println("\n\n=== E32-900T20D PARAMETER PERSISTENCE TEST ===");
  Serial.println("Mode 3 only, no RF. Run twice with a hard power cycle between runs.\n");

  uint32_t settledAfterMs = 0;
  if (!waitModuleReady(AUX_READY_TIMEOUT_MS, &settledAfterMs)) {
    Serial.println("  ABORT: AUX never settled high, module never reported ready");
    return;
  }
  Serial.printf("  AUX settled high %lu ms into setup\n\n", (unsigned long)settledAfterMs);

  Serial2.begin(BAUD_RATE_UART_LORA_E32900T20D_DEFAULT, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);
  delay(20);

  uint8_t boot[RESP_CONFIG_LEN] = {0};
  const uint8_t bootGot = readConfig(boot);
  printFrame("boot config", boot, bootGot);
  if (bootGot != RESP_CONFIG_LEN || boot[0] != CMD_WRITE_SAVED) {
    Serial.println("\n  ABORT: boot config read did not answer with a C0 frame");
    return;
  }

  memcpy(g_bootConfig, boot, RESP_CONFIG_LEN);
  g_haveBootConfig = true;
  Serial.println("\n  press 'w' to shift the address by one and write it with C0");
}

void loop() {
  if (!Serial.available()) {
    delay(20);
    return;
  }
  const int key = Serial.read();
  if (key == 'w' && g_haveBootConfig) {
    shiftAndWrite();
  }
}

