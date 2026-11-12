#include "../shared/round_safety.h"
#include <cassert>
#include <cstdio>

int main() {
  using namespace RoundSafety;
  // Reproduce the field failure: an otherwise reachable backend must not be
  // allowed to spend five seconds reconnecting during a 250 ms deadline margin.
  const Phase blocked[] = {Phase::WAIT_READY, Phase::CONFIGURED, Phase::PREPARED,
      Phase::ARMED, Phase::MEASURING, Phase::CHANNEL_WAIT, Phase::CANCELLING, Phase::FAILED};
  for (Phase phase : blocked) assert(!backendAllowed(phase, true, true, false));
  assert(timingCritical(Phase::ARMED) && timingCritical(Phase::MEASURING));
  assert(!timingCritical(Phase::COLLECTING));
  for (Phase phase : {Phase::IDLE, Phase::COLLECTING, Phase::REPORTING}) {
    assert(backendAllowed(phase, true, true, false));
    assert(!backendAllowed(phase, false, true, false));
    assert(!backendAllowed(phase, true, false, false));
    assert(!backendAllowed(phase, true, true, true));
  }

  Cancellation cancel;
  cancel.begin(4, 0, 100, 60000);
  assert(cancel.acknowledged(0));
  assert(cancel.status(101) == Cancellation::Status::Pending);
  assert(cancel.acknowledge(1));
  assert(cancel.acknowledge(1));  // Duplicate ACK cannot stand in for another node.
  assert(!cancel.acknowledge(4));
  cancel.acknowledge(2);
  assert(cancel.status(200) == Cancellation::Status::Pending);
  cancel.acknowledge(3);  // Last node returns after its already-started capture.
  assert(cancel.status(30000) == Cancellation::Status::Acknowledged);

  cancel.begin(4, 0, 500, 60000);  // New cancellation must discard old ACKs.
  assert(!cancel.acknowledged(3));
  cancel.acknowledge(1); cancel.acknowledge(2);
  assert(cancel.status(60499) == Cancellation::Status::Pending);
  assert(cancel.status(60500) == Cancellation::Status::TimedOut);  // Offline node never ACKs.
  cancel.begin(2, 0, 0xFFFFFF00u, 1000);
  assert(cancel.status(0xFFFFFF00u + 999u) == Cancellation::Status::Pending);
  assert(cancel.status(0xFFFFFF00u + 1000u) == Cancellation::Status::TimedOut);
  std::puts("Backend timing gates and cancellation recovery tests passed");
}
