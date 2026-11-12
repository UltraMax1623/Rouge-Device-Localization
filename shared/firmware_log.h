#pragma once

#if FIRMWARE_DEBUG
#include <Arduino.h>
#include <cstdio>
#include <cstring>

namespace FirmwareTrace {
constexpr size_t capacity = 24;
constexpr size_t lineSize = 192;
inline char lines[capacity][lineSize] = {};
inline size_t head = 0, count = 0, offset = 0;
inline unsigned dropped = 0;

template <typename... Args>
void append(const char *format, Args... args) {
  if (count == capacity) {
    ++dropped;
    return;
  }
  char *line = lines[(head + count) % capacity];
  const size_t prefix = std::snprintf(line, lineSize, "[%lu ms] [B%02u] ", millis(), unsigned(BOARD_NUMBER));
  std::snprintf(line + prefix, lineSize - prefix - 1, format, args...);
  size_t length = std::strlen(line);
  while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) --length;
  line[length++] = '\n';
  line[length] = '\0';
  ++count;
}

inline void log() {}
inline void log(const char *message) { append("%s", message); }
template <typename... Args>
void log(const char *format, Args... args) { append(format, args...); }

// Called from loop outside timing-sensitive radio phases. Limit writes to
// available space so an attached monitor cannot stall the measurement loop.
inline void flush() {
  const int available = Serial.availableForWrite();
  if (available <= 0) return;
  if (!count && dropped) {
    append("[TRACE] Dropped %u lines while the serial buffer was full", dropped);
    dropped = 0;
  }
  if (!count) return;
  const char *line = lines[head];
  const size_t remaining = std::strlen(line) - offset;
  const size_t bytes = remaining < size_t(available) ? remaining : size_t(available);
  offset += Serial.write(reinterpret_cast<const uint8_t *>(line + offset), bytes);
  if (!line[offset]) {
    head = (head + 1) % capacity;
    --count;
    offset = 0;
  }
}
}
#define FW_LOG(...) FirmwareTrace::log(__VA_ARGS__)
#else
#define FW_LOG(...) do {} while (0)
#endif
