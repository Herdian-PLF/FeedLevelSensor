// ESP32 <-> TMF8829 communication check for the silometer perfboard prototype.
//
// Proves the I2C link, the EN line and the identity of the part sitting at 0x41.
// It stops at the bootloader on purpose: no firmware is downloaded and no ranging
// is started, so a failure here implicates a wire rather than a driver.
//
// Register addresses and bit positions mirror the vendor Python driver
// (python-poc/driver/tmf8829/tmf8829_host_regs.py).

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"
#include "check_report.h"

namespace {

const uint8_t REG_INT_STATUS = 0xE1;
const uint8_t REG_ID = 0xE3;
const uint8_t REG_REVID = 0xE4;
const uint8_t REG_ENABLE = 0xF8;
const uint8_t REG_APP_ID = 0x00;

const uint8_t ID_EXPECTED = 0x9E;

const uint8_t ENABLE_PON = 0x04;
const uint8_t ENABLE_CPU_READY = 0x80;
const uint8_t ENABLE_POWERUP_BOOTMONITOR = 0x10;

const uint8_t APP_ID_BOOTLOADER = 0x80;
const uint8_t APP_ID_APPLICATION = 0x01;

// The vendor driver waits 3 ms after asserting enable and after a power-up write.
const uint32_t DEVICE_WAKE_MS = 5;

char text[160];
int intLevelEnLow = -1;
bool busIdleWithEnLow = false;
bool i2cStarted = false;

bool readRegs(uint8_t reg, uint8_t* buf, uint8_t count) {
  Wire.beginTransmission(TOF_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)TOF_I2C_ADDR, (int)count) != count) {
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    buf[i] = Wire.read();
  }
  return true;
}

bool readReg(uint8_t reg, uint8_t* value) { return readRegs(reg, value, 1); }

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOF_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool scanBus(char* out, size_t outSize) {
  bool targetPresent = false;
  uint8_t found = 0;
  uint8_t addrs[16];
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) {
      continue;
    }
    if (found < sizeof(addrs)) {
      addrs[found] = addr;
    }
    ++found;
    if (addr == TOF_I2C_ADDR) {
      targetPresent = true;
    }
  }

  if (found == 0) {
    snprintf(out, outSize, "bus scan: no devices responded");
    return false;
  }
  // Every address appearing to ACK means SDA is being held low, not that the bus
  // is crowded. Reporting the list would invite exactly the wrong conclusion.
  if (found > sizeof(addrs)) {
    snprintf(out, outSize, "bus scan: %u addresses ACKed - SDA is held low, scan is meaningless",
             found);
    return false;
  }
  size_t used = snprintf(out, outSize, "bus scan:");
  for (uint8_t i = 0; i < found && used < outSize; ++i) {
    used += snprintf(out + used, outSize - used, " 0x%02X", addrs[i]);
  }
  return targetPresent;
}

bool checkBusIdle() {
  pinMode(PIN_TOF_EN, OUTPUT);
  digitalWrite(PIN_TOF_EN, LOW);
  pinMode(PIN_TOF_INT, INPUT_PULLUP);
  pinMode(PIN_TOF_SDA, INPUT_PULLUP);
  pinMode(PIN_TOF_SCL, INPUT_PULLUP);
  delay(50);
  const int sdaEnLow = digitalRead(PIN_TOF_SDA);
  const int sclEnLow = digitalRead(PIN_TOF_SCL);
  busIdleWithEnLow = (sdaEnLow == HIGH && sclEnLow == HIGH);
  intLevelEnLow = digitalRead(PIN_TOF_INT);

  // The shield may not power the host side of its level shifter until EN is
  // asserted, so lines pulled low with EN low are not yet evidence of a fault.
  digitalWrite(PIN_TOF_EN, HIGH);
  delay(50);
  const int sda = digitalRead(PIN_TOF_SDA);
  const int scl = digitalRead(PIN_TOF_SCL);

  report.step("SDA/SCL idle high before Wire.begin");
  snprintf(text, sizeof(text), "EN-low SDA=%d SCL=%d / EN-high SDA=%d SCL=%d", sdaEnLow, sclEnLow,
           sda, scl);
  if (sda == HIGH && scl == HIGH) {
    report.pass(text);
    if (sdaEnLow == LOW || sclEnLow == LOW) {
      report.hint("lines only float high once EN is asserted - expected for this shield");
    }
    return true;
  }
  report.fail(text);
  report.hint("a line stuck low with EN high is a short to GND, or an unpowered shield");
  report.hint("every later step is meaningless until this is fixed");
  return false;
}

bool checkEnableGating() {
  char busText[160];

  digitalWrite(PIN_TOF_EN, LOW);
  delay(50);
  report.step("EN (IO25) gates the shield");
  if (!busIdleWithEnLow) {
    // The bus collapsing with EN low and recovering with EN high is itself proof
    // that EN reaches the shield, and it rules out a scan: a low SDA ACKs at
    // every address, which would read as a bus full of phantom devices.
    report.pass("bus low with EN low, high with EN high");
    report.hint("EN is reaching the shield D6 land and powering its host buffers");
    report.hint("address scan skipped with EN low - a held-low SDA ACKs everywhere");
  } else {
    const bool presentLow = scanBus(busText, sizeof(busText));
    if (!presentLow) {
      report.pass("0x41 silent with EN low");
    } else {
      report.fail("0x41 answers even with EN low");
      report.hint("EN is not reaching the shield D6 land, or sits strapped high");
      report.hint("the sensor is clearly alive - this is an EN wiring fault only");
    }
    report.hint(busText);
  }

  digitalWrite(PIN_TOF_EN, HIGH);
  delay(DEVICE_WAKE_MS);
  const bool presentHigh = scanBus(busText, sizeof(busText));
  report.step("0x41 responds while EN (IO25) is high");
  if (presentHigh) {
    report.pass();
  } else {
    report.fail("no ACK at 0x41");
    report.hint("check IO21=SDA (CN4-9) and IO22=SCL (CN4-10) are not swapped");
    report.hint("check the shield has 3V3 and GND on CN1");
    report.hint("if both lines idle high but nothing ACKs, fit 4k7 from SDA and SCL to 3V3");
  }
  report.hint(busText);
  return presentHigh;
}

void checkIdentity() {
  uint8_t id = 0;
  uint8_t revid = 0;
  const bool idOk = readReg(REG_ID, &id);
  const bool revOk = readReg(REG_REVID, &revid);

  report.step("device ID reads back 0x9E");
  if (!idOk || !revOk) {
    report.fail("register read did not complete");
    report.hint("the address ACKs but data transfer fails - suspect SDA timing or pull-ups");
    return;
  }
  snprintf(text, sizeof(text), "ID=0x%02X REVID=0x%02X", id, revid);
  if (id == ID_EXPECTED) {
    report.pass(text);
  } else if (id == 0x00 || id == 0xFF) {
    report.fail(text);
    report.hint("an all-zero or all-ones read means the data line is stuck, not a wrong part");
  } else {
    report.fail(text);
    report.hint("something ACKs at 0x41 but it does not identify as a TMF8829");
  }
}

void checkWritePath() {
  // Reading proves the address decodes; only a write that changes observable state
  // proves the host can actually drive SDA hard enough. Forcing the bootmonitor
  // matches the vendor wake-up sequence and leaves the part in a known state.
  const bool written = writeReg(REG_ENABLE, ENABLE_PON | ENABLE_POWERUP_BOOTMONITOR);
  delay(DEVICE_WAKE_MS);

  uint8_t enable = 0;
  const bool readBack = readReg(REG_ENABLE, &enable);

  report.step("power-on write takes effect (cpu_ready)");
  if (!written) {
    report.fail("ENABLE write was not acknowledged");
    report.hint("reads work but writes do not - classic weak pull-up symptom");
    return;
  }
  if (!readBack) {
    report.fail("ENABLE read-back failed");
    return;
  }
  snprintf(text, sizeof(text), "ENABLE=0x%02X", enable);
  if ((enable & ENABLE_CPU_READY) != 0 && (enable & ENABLE_PON) != 0) {
    report.pass(text);
  } else {
    report.fail(text);
    report.hint("the write landed but the CPU never reported ready - check the shield 3V3 rail");
  }
}

void checkAppId() {
  uint8_t app[4] = {0, 0, 0, 0};
  report.step("application ID block readable");
  if (!readRegs(REG_APP_ID, app, sizeof(app))) {
    report.fail("multi-byte read failed");
    report.hint("single-byte reads worked, so suspect clock stretching or bus speed");
    return;
  }
  snprintf(text, sizeof(text), "APP_ID=0x%02X MAJOR=%u MINOR=%u CAP=0x%02X", app[0], app[1],
           app[2], app[3]);
  if (app[0] == APP_ID_BOOTLOADER) {
    report.pass(text);
    report.hint("0x80 is the bootloader - expected, this app downloads no firmware");
  } else if (app[0] == APP_ID_APPLICATION) {
    report.pass(text);
    report.hint("0x01 means an application is already running in RAM");
  } else {
    report.fail(text);
  }
}

void observeInterrupt() {
  uint8_t intStatus = 0;
  const bool statusOk = readReg(REG_INT_STATUS, &intStatus);
  const int level = digitalRead(PIN_TOF_INT);

  report.step("INT (IO26) observation");
  snprintf(text, sizeof(text), "level EN-low=%d EN-high=%d INT_STATUS=%s", intLevelEnLow, level,
           statusOk ? "read" : "unreadable");
  report.observed(text);
  report.hint("NOT a pass/fail: asserting INT needs the application firmware booted,");
  report.hint("which is out of scope here. Verify INT when the download path exists.");
  if (statusOk) {
    snprintf(text, sizeof(text), "INT_STATUS=0x%02X (active low pin, high = no pending int)",
             intStatus);
    report.hint(text);
  }
}

}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(500);

  report.begin("SILOMETER HW CHECK  -  ESP32 <-> TMF8829 over I2C");
  report.note("USB-powered test. The battery at J1 must be unplugged.");
  report.note("No firmware download, no ranging - communication only.");
  report.blank();

  if (!checkBusIdle()) {
    report.note("scan and register checks skipped - the bus is held low");
  } else {
    Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL, TOF_I2C_HZ);
    i2cStarted = true;
    if (checkEnableGating()) {
      checkIdentity();
      checkWritePath();
      checkAppId();
      observeInterrupt();
    } else {
      report.note("register checks skipped - nothing is answering at 0x41");
    }
  }

  report.summary();
  report.note("live INT/presence line follows; wiggle a lead to watch it react");
}

void loop() {
  if (i2cStarted) {
    Wire.beginTransmission(TOF_I2C_ADDR);
    const bool present = Wire.endTransmission() == 0;
    Serial.printf("  0x41 %-7s   INT(IO26)=%d\n", present ? "present" : "absent",
                  digitalRead(PIN_TOF_INT));
  } else {
    Serial.printf("  SDA=%d SCL=%d   INT(IO26)=%d\n", digitalRead(PIN_TOF_SDA),
                  digitalRead(PIN_TOF_SCL), digitalRead(PIN_TOF_INT));
  }
  delay(1000);
}
