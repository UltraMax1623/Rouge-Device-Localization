#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace TimeSliceProtocol {
constexpr uint8_t CHANNEL = 1;
constexpr uint32_t GUARD_US = 500000;
constexpr uint32_t SAMPLE_US = 3000000;
constexpr uint32_t SLOT_US = SAMPLE_US + 2 * GUARD_US;
constexpr uint32_t PREPARE_US = 3000000;
constexpr uint32_t START_LEAD_US = 10000000;
constexpr size_t PAYLOAD_SIZE = 18;
constexpr int64_t DISCOVERY_US = 10000000;
constexpr int64_t SNIFF_US = 20000000;
constexpr uint8_t SNIFF_CYCLES = 3;
constexpr int64_t CHANNEL_DWELL_US = 200000;

inline int64_t sniffDurationUs(bool discovery) { return discovery ? DISCOVERY_US : SNIFF_US; }
inline uint8_t nextSniffCycle(uint8_t cycle) {
  return cycle < SNIFF_CYCLES ? uint8_t(cycle + 1) : 0;
}

inline uint8_t sniffChannel(int64_t elapsedUs, uint8_t first, uint8_t count) {
  return uint8_t(first + (elapsedUs < 0 ? 0 : elapsedUs / CHANNEL_DWELL_US) % count);
}

// Address 2 is the physical transmitter in management and data frames,
// including ToDS/FromDS and four-address frames. Never match address 3/4
// (which may describe an original source behind a radio relay).
inline bool targetFrame(const uint8_t *frame, size_t length,
                        const uint8_t *target) {
  if (length < 28 || (frame[0] & 0x03) != 0) return false;  // Header + FCS.
  const uint8_t type = (frame[0] >> 2) & 3;
  if (type != 0 && type != 2) return false;
  if (type == 2 && (frame[1] & 3) == 3 && length < 34) return false;
  return memcmp(frame + 10, target, 6) == 0;
}

inline int32_t remainingUs(uint32_t target, uint32_t now) {
  return static_cast<int32_t>(target - now);
}

inline uint32_t read32(const uint8_t *bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

inline void write32(uint8_t *bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[i] = value >> (24 - 8 * i);
}

inline void makePayload(uint8_t *bytes, uint32_t round, uint32_t hash,
                        uint16_t slot, uint32_t sequence) {
  memcpy(bytes, "RSL1", 4);
  write32(bytes + 4, round);
  write32(bytes + 8, hash);
  bytes[12] = slot >> 8;
  bytes[13] = slot & 0xFF;
  write32(bytes + 14, sequence);
}

// sig_len includes the four-byte FCS. Only accept our short, unfragmented
// ESP-NOW action frames, not arbitrary vendor management traffic.
inline const uint8_t *measurementPayload(const uint8_t *frame, size_t size) {
  constexpr size_t BODY_OFFSET = 39;
  if (size < BODY_OFFSET + PAYLOAD_SIZE + 4 ||
      (frame[0] & 0xFC) != 0xD0 || (frame[1] & 0x03) != 0 ||
      frame[24] != 127 || frame[25] != 0x18 ||
      frame[26] != 0xFE || frame[27] != 0x34 ||
      frame[32] != 221 || frame[33] != 5 + PAYLOAD_SIZE ||
      frame[34] != 0x18 || frame[35] != 0xFE || frame[36] != 0x34 ||
      frame[37] != 4) return nullptr;
  uint8_t version = frame[38] & 0x0F;
  if ((version != 1 && version != 2) ||
      (version == 2 && (frame[38] & 0x10)) ||
      memcmp(frame + BODY_OFFSET, "RSL1", 4) != 0) return nullptr;
  return frame + BODY_OFFSET;
}

inline bool matchesMeasurement(const uint8_t *payload, const uint8_t *source,
                               const uint8_t *expectedSource, uint32_t round,
                               uint32_t hash, size_t slot, uint32_t lastSequence) {
  uint16_t advertisedSlot = (uint16_t(payload[12]) << 8) | payload[13];
  return read32(payload + 4) == round && read32(payload + 8) == hash &&
         advertisedSlot == slot && memcmp(source, expectedSource, 6) == 0 &&
         read32(payload + 14) > lastSequence;
}

inline bool activeSlot(int64_t now, int64_t start, size_t count,
                       size_t &slot) {
  if (now < start) return false;
  uint64_t elapsed = static_cast<uint64_t>(now - start);
  slot = elapsed / SLOT_US;
  uint32_t offset = elapsed % SLOT_US;
  return slot < count && offset >= GUARD_US &&
         offset < SLOT_US - GUARD_US;
}
}  // namespace TimeSliceProtocol
