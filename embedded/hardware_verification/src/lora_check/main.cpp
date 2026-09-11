// ESP32 <-> EBYTE E32-900T20D communication check for the silometer perfboard prototype.
//
// Exercises the UART pair, both mode pins and AUX by reading the module's own
// version and configuration registers. The module is only ever put into Mode 3
// (sleep/config), which emits no RF - this is safe to run before the 915 MHz
// antenna is fitted, and it must stay that way.

#include <Arduino.h>

#include "board_pins.h"
#include "check_report.h"

namespace {

const uint8_t CMD_READ_VERSION = 0xC3;
const uint8_t CMD_READ_CONFIG = 0xC1;
const uint8_t RESP_VERSION_LEN = 4;
const uint8_t RESP_CONFIG_LEN = 6;
const uint8_t RESP_CONFIG_HEAD = 0xC0;

const uint32_t AUX_SETTLE_MS = 200;
const uint32_t RESPONSE_TIMEOUT_MS = 1000;

// A module left configured at another baud is otherwise indistinguishable from a
// broken wire, so the default is tried first and the rest only after a timeout.
const uint32_t BAUD_CANDIDATES[] = {9600, 115200, 57600, 38400, 19200, 4800, 2400, 1200};

const char* const UART_BAUD_NAMES[] = {"1200",  "2400",  "4800",  "9600",
                                       "19200", "38400", "57600", "115200"};
const char* const AIR_RATE_NAMES[] = {"0.3k", "1.2k", "2.4k", "4.8k",
                                      "9.6k", "19.2k", "19.2k", "19.2k"};
const char* const PARITY_NAMES[] = {"8N1", "8O1", "8E1", "8N1"};
const char* const TX_POWER_NAMES[] = {"20dBm", "17dBm", "14dBm", "10dBm"};

// Manual section 7.3: the byte after the C3 prefix is the PRODUCT MODEL code,
// not a frequency code. Section 7.5: carrier = 862 MHz + CHAN, CHAN 0x00-0x45,
// factory default 0x06 = 868 MHz.
const uint8_t MODEL_CODE_E32_900T20D = 0x32;
const uint16_t CHAN_BASE_MHZ = 862;
const uint8_t CHAN_MAX = 0x45;

// ANATEL grants 902-907.5 and 915-928 MHz. The module ships on the EU default.
const uint16_t ANATEL_LOW_MIN = 902;
const uint16_t ANATEL_LOW_MAX = 907;
const uint16_t ANATEL_HIGH_MIN = 915;
const uint16_t ANATEL_HIGH_MAX = 928;
const uint8_t CHAN_FOR_915 = 0x35;

const char* interfaceName(uint8_t features) {
  switch (features & 0xF0) {
    case 0x10:
      return "TTL";
    case 0x40:
      return "RS232";
    case 0x80:
      return "RS485";
    default:
      return "unknown interface";
  }
}

char text[160];
uint32_t activeBaud = LORA_DEFAULT_BAUD;
bool auxEverDipped = false;

void setMode(uint8_t m0, uint8_t m1) {
  digitalWrite(PIN_LORA_M0, m0);
  digitalWrite(PIN_LORA_M1, m1);
}

bool waitAuxHigh(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (digitalRead(PIN_LORA_AUX) == HIGH) {
      return true;
    }
    delay(1);
  }
  return false;
}

// AUX drops for a few milliseconds on a mode change. Catching that edge is the
// only thing that distinguishes a wired AUX from one floating on the pull-up.
uint32_t enterModeWatchingAux(uint8_t m0, uint8_t m1) {
  uint32_t lowSamples = 0;
  setMode(m0, m1);
  const uint32_t deadline = millis() + AUX_SETTLE_MS;
  while (millis() < deadline) {
    if (digitalRead(PIN_LORA_AUX) == LOW) {
      ++lowSamples;
      auxEverDipped = true;
    }
  }
  return lowSamples;
}

void openUart(uint32_t baud) {
  Serial2.end();
  Serial2.begin(baud, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);
  delay(20);
  while (Serial2.available()) {
    Serial2.read();
  }
}

uint8_t sendCommand(uint8_t command, uint8_t* buf, uint8_t expected) {
  while (Serial2.available()) {
    Serial2.read();
  }
  for (uint8_t i = 0; i < 3; ++i) {
    Serial2.write(command);
  }
  Serial2.flush();

  uint8_t got = 0;
  const uint32_t deadline = millis() + RESPONSE_TIMEOUT_MS;
  while (got < expected && millis() < deadline) {
    if (Serial2.available()) {
      buf[got++] = (uint8_t)Serial2.read();
    }
  }
  return got;
}

void appendHex(char* out, size_t outSize, const uint8_t* buf, uint8_t len) {
  size_t used = 0;
  for (uint8_t i = 0; i < len && used < outSize; ++i) {
    used += snprintf(out + used, outSize - used, "%02X ", buf[i]);
  }
}

void checkAuxBaseline() {
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  // Pull-up rather than pull-down so an open-drain AUX configuration still works;
  // a disconnected AUX should therefore read high and be caught by the transient check.
  pinMode(PIN_LORA_AUX, INPUT_PULLUP);
  setMode(LOW, LOW);
  delay(100);

  report.step("AUX (IO4) idles high in Mode 0");
  if (waitAuxHigh(AUX_SETTLE_MS)) {
    report.pass();
  } else {
    report.fail("AUX stays low");
    report.hint("a permanently low AUX means the module is held busy or not powered");
    report.hint("check E32 pin 6 VCC and pin 7 GND");
  }
}

void checkModeTransition() {
  const uint32_t lowSamples = enterModeWatchingAux(HIGH, HIGH);
  waitAuxHigh(AUX_SETTLE_MS);

  report.step("AUX reacts to the Mode 3 transition");
  snprintf(text, sizeof(text), "%lu low samples during switch", (unsigned long)lowSamples);
  if (lowSamples > 0) {
    report.pass(text);
  } else {
    report.warn("no AUX activity seen");
    report.hint("AUX may be disconnected - the pull-up alone would also read high");
    report.hint("not fatal: the dip is short and can be missed, the reads below still apply");
  }
}

bool readVersion(uint8_t* buf) {
  for (size_t i = 0; i < sizeof(BAUD_CANDIDATES) / sizeof(BAUD_CANDIDATES[0]); ++i) {
    openUart(BAUD_CANDIDATES[i]);
    waitAuxHigh(AUX_SETTLE_MS);
    const uint8_t got = sendCommand(CMD_READ_VERSION, buf, RESP_VERSION_LEN);
    if (got == RESP_VERSION_LEN && buf[0] == CMD_READ_VERSION) {
      activeBaud = BAUD_CANDIDATES[i];
      return true;
    }
  }
  return false;
}

void checkVersion(bool ok, const uint8_t* buf) {
  report.step("version read (C3 C3 C3) answers");
  if (!ok) {
    report.fail("no valid reply at any baud");
    report.hint("this one exchange needs TX, RX, M0 and M1 all correct");
    report.hint("check the crossover: IO17 -> E32 pin 3 RXD, IO16 <- E32 pin 4 TXD");
    report.hint("wiring TXD to TXD is the usual cause of total silence");
    return;
  }
  snprintf(text, sizeof(text), "%lu baud", (unsigned long)activeBaud);
  report.pass(text);
  if (activeBaud != LORA_DEFAULT_BAUD) {
    report.hint("module is NOT at its 9600 default - it has been reconfigured before");
  }

  char hex[64] = {0};
  appendHex(hex, sizeof(hex), buf, RESP_VERSION_LEN);
  snprintf(text, sizeof(text), "raw: %s", hex);
  report.hint(text);
}

void checkModel(const uint8_t* buf) {
  report.step("module identifies as E32-900T20D");
  snprintf(text, sizeof(text), "model=0x%02X version=0x%02X %s", buf[1], buf[2],
           interfaceName(buf[3]));
  if (buf[1] == MODEL_CODE_E32_900T20D) {
    report.pass(text);
  } else {
    report.warn(text);
    report.hint("expected model code 0x32 - check the label on the module");
    report.hint("this is a part finding, not a wiring fault");
  }
}

void checkChannel(uint8_t chan) {
  report.step("carrier sits in an ANATEL band");
  if (chan > CHAN_MAX) {
    snprintf(text, sizeof(text), "CHAN=0x%02X is above the 0x45 maximum", chan);
    report.warn(text);
    return;
  }
  const uint16_t mhz = CHAN_BASE_MHZ + chan;
  snprintf(text, sizeof(text), "CHAN=0x%02X -> %u MHz", chan, mhz);
  const bool inBand = (mhz >= ANATEL_LOW_MIN && mhz <= ANATEL_LOW_MAX) ||
                      (mhz >= ANATEL_HIGH_MIN && mhz <= ANATEL_HIGH_MAX);
  if (inBand) {
    report.pass(text);
    return;
  }
  report.warn(text);
  report.hint("ANATEL grants 902-907.5 and 915-928 MHz; 868 MHz is the EU factory default");
  report.hint("set CHAN=0x35 (862 + 53) for 915 MHz before transmitting in Brazil");
  report.hint("this is a configuration finding, not a wiring fault");
}

void decodeConfig(const uint8_t* cfg) {
  const uint8_t sped = cfg[3];
  const uint8_t chan = cfg[4];
  const uint8_t option = cfg[5];

  snprintf(text, sizeof(text), "address=0x%02X%02X channel=0x%02X (%u MHz)", cfg[1], cfg[2], chan,
           (unsigned)(CHAN_BASE_MHZ + chan));
  report.hint(text);
  snprintf(text, sizeof(text), "uart=%s %s  air rate=%s", UART_BAUD_NAMES[(sped >> 3) & 0x07],
           PARITY_NAMES[(sped >> 6) & 0x03], AIR_RATE_NAMES[sped & 0x07]);
  report.hint(text);
  snprintf(text, sizeof(text), "%s mode, %s drive, FEC %s, tx power %s",
           (option & 0x80) ? "fixed" : "transparent", (option & 0x40) ? "push-pull" : "open-drain",
           (option & 0x04) ? "on" : "off", TX_POWER_NAMES[option & 0x03]);
  report.hint(text);
}

void checkConfig() {
  uint8_t cfg[RESP_CONFIG_LEN] = {0};
  openUart(activeBaud);
  waitAuxHigh(AUX_SETTLE_MS);
  const uint8_t got = sendCommand(CMD_READ_CONFIG, cfg, RESP_CONFIG_LEN);

  report.step("config read (C1 C1 C1) answers");
  if (got != RESP_CONFIG_LEN || cfg[0] != RESP_CONFIG_HEAD) {
    snprintf(text, sizeof(text), "got %u/%u bytes, head=0x%02X", got, RESP_CONFIG_LEN, cfg[0]);
    report.fail(text);
    report.hint("the version read worked, so the link is up but the reply is malformed");
    return;
  }
  report.pass();

  char hex[64] = {0};
  appendHex(hex, sizeof(hex), cfg, RESP_CONFIG_LEN);
  snprintf(text, sizeof(text), "raw: %s", hex);
  report.hint(text);
  // Decoded fields are advisory: a stale lookup table must never be reported as a
  // hardware fault, so only the header byte and the frame length gate the verdict.
  decodeConfig(cfg);
  checkChannel(cfg[4]);
}

void restoreNormalMode() {
  enterModeWatchingAux(LOW, LOW);
  report.step("returned to Mode 0 with AUX high");
  if (waitAuxHigh(AUX_SETTLE_MS)) {
    report.pass("module left in normal transmission mode");
  } else {
    report.fail("AUX did not return high");
  }
}

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(500);

  report.begin("SILOMETER HW CHECK  -  ESP32 <-> E32-900T20D over UART2");
  report.note("USB-powered test. The battery at J1 must be unplugged.");
  report.note("Mode 3 only - this app transmits no RF and is safe without an antenna.");
  report.blank();

  checkAuxBaseline();
  checkModeTransition();

  uint8_t version[RESP_VERSION_LEN] = {0};
  const bool versionOk = readVersion(version);
  checkVersion(versionOk, version);
  if (versionOk) {
    checkModel(version);
    checkConfig();
  } else {
    report.note("config read skipped - the module is not answering");
  }

  restoreNormalMode();
  report.summary();
  if (!auxEverDipped) {
    report.note("AUX never moved during the run - treat its wiring as unconfirmed");
  }
  report.note("live AUX line follows; wiggle a lead to watch it react");
}

void loop() {
  Serial.printf("  AUX(IO4)=%d   M0=%d M1=%d (Mode 0)\n", digitalRead(PIN_LORA_AUX),
                digitalRead(PIN_LORA_M0), digitalRead(PIN_LORA_M1));
  delay(1000);
}
