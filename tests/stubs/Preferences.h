#pragma once
#include <vector>
#include <cstring>
// In-memory NVS substitute for calibration lifecycle tests only.
static std::vector<unsigned char> testNvs;
static bool testNvsWriteFailure = false;
class Preferences {
 public:
  bool begin(const char *, bool) { return true; }
  void end() {}
  size_t getBytesLength(const char *) { return testNvs.size(); }
  size_t getBytes(const char *, void *dest, size_t size) {
    if (size != testNvs.size()) return 0;
    std::memcpy(dest, testNvs.data(), size); return size;
  }
  size_t putBytes(const char *, const void *src, size_t size) {
    if (testNvsWriteFailure) return 0;
    const auto *bytes = static_cast<const unsigned char *>(src);
    testNvs.assign(bytes, bytes + size); return size;
  }
};
