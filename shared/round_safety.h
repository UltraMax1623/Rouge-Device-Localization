#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace RoundSafety {
enum class Phase { IDLE, WAIT_READY, CONFIGURED, PREPARED, ARMED,
                   MEASURING, COLLECTING, REPORTING, CHANNEL_WAIT, CANCELLING, FAILED };

inline bool timingCritical(Phase phase) {
  return phase == Phase::ARMED || phase == Phase::MEASURING;
}

inline bool backendAllowed(Phase phase, bool meshStarted, bool routerConnected,
                           bool calibrationActive) {
  if (!meshStarted || !routerConnected || calibrationActive) return false;
  // Waiting/redistribution can arm the next round on the next controller
  // update. Only perform potentially blocking backend I/O outside that path.
  return phase == Phase::IDLE || phase == Phase::COLLECTING || phase == Phase::REPORTING;
}

class Cancellation {
 public:
  enum class Status { Pending, Acknowledged, TimedOut };
  void begin(size_t nodes, size_t self, uint32_t now, uint32_t timeout) {
    acknowledged_.assign(nodes, false);
    if (self < nodes) acknowledged_[self] = true;
    started_ = now;
    timeout_ = timeout;
  }
  bool acknowledge(size_t slot) {
    if (slot >= acknowledged_.size()) return false;
    acknowledged_[slot] = true;
    return true;
  }
  bool acknowledged(size_t slot) const {
    return slot < acknowledged_.size() && acknowledged_[slot];
  }
  Status status(uint32_t now) const {
    bool all = !acknowledged_.empty();
    for (bool ack : acknowledged_) all &= ack;
    if (all) return Status::Acknowledged;
    return uint32_t(now - started_) >= timeout_ ? Status::TimedOut : Status::Pending;
  }
 private:
  std::vector<bool> acknowledged_;
  uint32_t started_ = 0, timeout_ = 10000;
};
}  // namespace RoundSafety
