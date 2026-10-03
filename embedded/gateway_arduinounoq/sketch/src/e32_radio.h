// EBYTE E32-900T20D driver for the UNO Q microcontroller: mode switching,
// parameter verification, and framed send/receive in fixed-transmission mode.
// Register semantics are those of E32-900T20D_UserManual_EN_v1.3, sections 6
// and 7. Ported from embedded/silometer_endpoint/lib/e32/, which runs the same
// module against ESP-IDF; the two differ only in the UART and the logging.

#pragma once

#include <stdint.h>

#include "protocol/protocol.h"

namespace gw {

enum class E32Mode : uint8_t {
  Normal = 0,
  WakeUp = 1,
  PowerSave = 2,
  Sleep = 3,
};

struct E32Config {
  uint16_t address;
  uint8_t sped;
  uint8_t channel;
  uint8_t option;
};

enum class E32Rx : int8_t {
  Ok = 0,
  Timeout = -1,
  Malformed = -2,
};

class E32Radio {
 public:
  bool begin();

  bool readConfig(E32Config* out);
  bool ensureConfig(const E32Config& wanted);

  bool setMode(E32Mode mode);
  bool sendTo(uint16_t dstAddress, uint8_t dstChannel, const uint8_t* frame, uint8_t len);

  // Collects bytes until one frame passes magic, length and CRC, or the window
  // closes. A frame that fails any check is reported without ending the window.
  E32Rx receive(proto::Frame* out, uint8_t* store, uint8_t storeLen, uint32_t timeoutMs);

 private:
  bool waitAuxStable(uint32_t stableMs, uint32_t timeoutMs);
  bool resetModule();
  bool sendCommand(const uint8_t* cmd, uint8_t len);
  uint8_t readAnswer(uint8_t* buf, uint8_t expected, uint32_t timeoutMs);
  void drain();

  proto::FrameReader reader_;
  E32Mode mode_ = E32Mode::Sleep;
};

}  // namespace gw
