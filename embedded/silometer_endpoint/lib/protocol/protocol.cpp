#include "protocol.h"

#include <string.h>

namespace proto {
namespace {

void putU16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

uint16_t getU16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

uint8_t finish(uint8_t* out, Type type, uint16_t src, uint8_t seq, uint8_t fragIndex,
               uint8_t fragCount, uint8_t payloadLen) {
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = (uint8_t)((kVersion << 4) | ((uint8_t)type & 0x0F));
  putU16(out + 3, src);
  out[5] = seq;
  out[6] = (uint8_t)((fragIndex << 4) | (fragCount & 0x0F));
  out[7] = payloadLen;
  const uint8_t total = (uint8_t)(kHeaderBytes + payloadLen);
  putU16(out + total, crc16(out, total));
  return (uint8_t)(total + kCrcBytes);
}

}  // namespace

uint16_t crc16(const uint8_t* data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

uint8_t zonesInFragment(uint8_t fragIndex) {
  if (fragIndex >= kFragmentCount) {
    return 0;
  }
  const uint8_t first = firstZoneOfFragment(fragIndex);
  const uint8_t remaining = (uint8_t)(cfg::kZoneCount - first);
  return remaining < kZonesPerFragment ? remaining : kZonesPerFragment;
}

uint8_t firstZoneOfFragment(uint8_t fragIndex) {
  return (uint8_t)(fragIndex * kZonesPerFragment);
}

uint8_t encodeHello(uint16_t src, uint8_t seq, const HelloInfo& info, uint8_t* out, uint8_t max) {
  if (max < kHeaderBytes + kHelloBytes + kCrcBytes) {
    return 0;
  }
  uint8_t* p = out + kHeaderBytes;
  p[0] = info.fragCount;
  p[1] = info.zonesTotal;
  p[2] = info.zonesPerFragment;
  p[3] = info.flags;
  p[4] = info.temperatureC;
  p[5] = info.validZones;
  putU16(p + 6, info.frameNumberLo);
  putU16(p + 8, info.bootCount);
  p[10] = info.resetReason;
  p[11] = info.consecutiveFailedCycles;
  p[12] = info.fwVersion;
  return finish(out, Type::Hello, src, seq, 0, 0, kHelloBytes);
}

uint8_t encodeHelloAck(uint16_t src, uint8_t seq, HelloStatus status, uint8_t* out, uint8_t max) {
  if (max < kHeaderBytes + 1 + kCrcBytes) {
    return 0;
  }
  out[kHeaderBytes] = (uint8_t)status;
  return finish(out, Type::HelloAck, src, seq, 0, 0, 1);
}

uint8_t encodeDataFragment(uint16_t src, uint8_t seq, uint8_t fragIndex,
                           const uint16_t* distanceMm, const uint8_t* snr, uint8_t zoneCount,
                           uint8_t* out, uint8_t max) {
  const uint8_t zones = zonesInFragment(fragIndex);
  if (zones == 0 || zoneCount < cfg::kZoneCount) {
    return 0;
  }
  const uint8_t payloadLen = (uint8_t)(zones * kZoneBytes);
  if (max < kHeaderBytes + payloadLen + kCrcBytes) {
    return 0;
  }
  const uint8_t first = firstZoneOfFragment(fragIndex);
  uint8_t* p = out + kHeaderBytes;
  for (uint8_t i = 0; i < zones; ++i) {
    putU16(p + i * kZoneBytes, distanceMm[first + i]);
    p[i * kZoneBytes + 2] = snr[first + i];
  }
  return finish(out, Type::Data, src, seq, fragIndex, kFragmentCount, payloadLen);
}

uint8_t encodeDataAck(uint16_t src, uint8_t seq, uint16_t receivedMask, const uint8_t* configTlv,
                      uint8_t configLen, uint8_t* out, uint8_t max) {
  const uint8_t payloadLen = (uint8_t)(3 + configLen);
  if (payloadLen > kMaxPayloadBytes || max < kHeaderBytes + payloadLen + kCrcBytes) {
    return 0;
  }
  uint8_t* p = out + kHeaderBytes;
  putU16(p, receivedMask);
  p[2] = configLen;
  if (configLen > 0) {
    memcpy(p + 3, configTlv, configLen);
  }
  return finish(out, Type::DataAck, src, seq, 0, 0, payloadLen);
}

Decode decode(const uint8_t* in, uint8_t len, Frame* out) {
  if (len < kOverheadBytes) {
    return Decode::TooShort;
  }
  if (in[0] != kMagic0 || in[1] != kMagic1) {
    return Decode::BadMagic;
  }
  if ((in[2] >> 4) != kVersion) {
    return Decode::BadVersion;
  }
  const uint8_t payloadLen = in[7];
  const uint16_t total = (uint16_t)kHeaderBytes + payloadLen;
  if (total + kCrcBytes != len) {
    return Decode::BadLength;
  }
  if (getU16(in + total) != crc16(in, total)) {
    return Decode::BadCrc;
  }
  out->type = (Type)(in[2] & 0x0F);
  out->src = getU16(in + 3);
  out->seq = in[5];
  out->fragIndex = (uint8_t)(in[6] >> 4);
  out->fragCount = (uint8_t)(in[6] & 0x0F);
  out->payload = in + kHeaderBytes;
  out->len = payloadLen;
  return Decode::Ok;
}

bool parseHello(const Frame& frame, HelloInfo* out) {
  if (frame.type != Type::Hello || frame.len < kHelloBytes) {
    return false;
  }
  const uint8_t* p = frame.payload;
  out->fragCount = p[0];
  out->zonesTotal = p[1];
  out->zonesPerFragment = p[2];
  out->flags = p[3];
  out->temperatureC = p[4];
  out->validZones = p[5];
  out->frameNumberLo = getU16(p + 6);
  out->bootCount = getU16(p + 8);
  out->resetReason = p[10];
  out->consecutiveFailedCycles = p[11];
  out->fwVersion = p[12];
  return true;
}

bool parseHelloAck(const Frame& frame, HelloStatus* out) {
  if (frame.type != Type::HelloAck || frame.len < 1) {
    return false;
  }
  *out = (HelloStatus)frame.payload[0];
  return true;
}

bool parseDataAck(const Frame& frame, uint16_t* receivedMask, const uint8_t** configTlv,
                  uint8_t* configLen) {
  if (frame.type != Type::DataAck || frame.len < 3) {
    return false;
  }
  const uint8_t declared = frame.payload[2];
  if (3 + declared > frame.len) {
    return false;
  }
  *receivedMask = getU16(frame.payload);
  *configTlv = frame.payload + 3;
  *configLen = declared;
  return true;
}

bool parseDataFragment(const Frame& frame, uint16_t* distanceMm, uint8_t* snr,
                       uint8_t zoneCapacity, uint8_t* zonesWritten) {
  if (frame.type != Type::Data || frame.fragIndex >= kFragmentCount) {
    return false;
  }
  const uint8_t zones = zonesInFragment(frame.fragIndex);
  if (frame.len != zones * kZoneBytes) {
    return false;
  }
  const uint8_t first = firstZoneOfFragment(frame.fragIndex);
  if (first + zones > zoneCapacity) {
    return false;
  }
  for (uint8_t i = 0; i < zones; ++i) {
    distanceMm[first + i] = getU16(frame.payload + i * kZoneBytes);
    snr[first + i] = frame.payload[i * kZoneBytes + 2];
  }
  *zonesWritten = zones;
  return true;
}

bool parseConfigTlv(const uint8_t* tlv, uint8_t len, ConfigUpdate* out) {
  *out = ConfigUpdate{};
  uint8_t i = 0;
  while (i < len) {
    const uint8_t tag = tlv[i++];
    if (tag == kTlvNop) {
      continue;
    }
    if (i >= len) {
      return false;
    }
    const uint8_t vlen = tlv[i++];
    if (i + vlen > len) {
      return false;
    }
    const uint8_t* v = tlv + i;
    switch (tag) {
      case kTlvReportInterval:
        if (vlen != 2) {
          return false;
        }
        out->hasReportInterval = true;
        out->reportIntervalS = getU16(v);
        break;
      case kTlvChannel:
        if (vlen != 1) {
          return false;
        }
        out->hasChannel = true;
        out->channel = v[0];
        break;
      case kTlvTofFrames:
        if (vlen != 1) {
          return false;
        }
        out->hasTofFrames = true;
        out->tofFrames = v[0];
        break;
      case kTlvReboot:
        out->reboot = true;
        break;
      default:
        break;  // forward compatibility: an unknown tag is skipped, not fatal
    }
    i = (uint8_t)(i + vlen);
  }
  return true;
}

void FrameReader::reset() {
  len_ = 0;
  expected_ = 0;
  ready_ = 0;
}

bool FrameReader::feed(uint8_t byte) {
  if (len_ == 0) {
    if (byte != kMagic0) {
      return false;
    }
  } else if (len_ == 1 && byte != kMagic1) {
    // A doubled magic0 is the start of a real frame, not a failed match.
    len_ = (byte == kMagic0) ? 1 : 0;
    return false;
  }

  buf_[len_++] = byte;

  if (len_ == kHeaderBytes) {
    const uint16_t total = (uint16_t)kHeaderBytes + buf_[7] + kCrcBytes;
    if (total > kMaxFrameBytes) {
      reset();
      return false;
    }
    expected_ = (uint8_t)total;
  }

  if (expected_ != 0 && len_ == expected_) {
    ready_ = len_;
    len_ = 0;
    expected_ = 0;
    return true;
  }
  if (len_ >= kMaxFrameBytes) {
    reset();
  }
  return false;
}

}  // namespace proto
