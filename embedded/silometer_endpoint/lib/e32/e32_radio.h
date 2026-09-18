// EBYTE E32-900T20D driver: mode switching, parameter verification, and framed
// send/receive in fixed-transmission mode. Register semantics are those of
// E32-900T20D_UserManual_EN_v1.3, sections 6 and 7.
//
// Fixed transmission (OPTION bit 7) is what gives the star network its hardware
// address filter: the first three bytes written to the module are the target
// address and channel, which the module consumes as routing and reverts after.
// A downlink addressed to one endpoint never reaches another endpoint's UART.

#pragma once

#include <stdint.h>

#include "protocol.h"

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
  void end();

  bool readConfig(E32Config* out);
  bool ensureConfig(const E32Config& wanted);

  bool setMode(E32Mode mode);
  bool sendTo(uint16_t dstAddress, uint8_t dstChannel, const uint8_t* frame, uint8_t len);

  // Collects bytes until one frame passes magic, length and CRC, or the window
  // closes. A frame that fails any check is reported without ending the window.
  E32Rx receive(proto::Frame* out, uint8_t* store, uint8_t storeLen, uint32_t timeoutMs);

  void sleep();

 private:
  bool waitAux(uint32_t timeoutMs);
  void drain();

  proto::FrameReader reader_;
  E32Mode mode_ = E32Mode::Sleep;
};
