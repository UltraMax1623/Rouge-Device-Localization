#include "../root_node/time_slicing_protocol.h"
#include "../shared/rssi_distance.h"
#include "../shared/serial_commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

double A = -60.0;
double n = 2.0;

int main() {
  using UserCommand::Kind;
  assert(UserCommand::parse("conf").kind == Kind::Conf);
  assert(UserCommand::parse("  conf \t").kind == Kind::Conf);
  const auto sniff = UserCommand::parse("sniff 112233445566");
  assert(sniff.kind == Kind::Sniff && sniff.channel == 0 && sniff.target[5] == 0x66);
  assert(UserCommand::parse("sniff aabbccddeeff 6").channel == 6);
  assert(UserCommand::parse("sniff AABBCCDDEEFF 13").channel == 13);
  const char *invalidCommands[] = {"", "config", "conf extra", "sniff", "sniff 11",
      "sniff 11223344556g", "sniff 1122334455667", "sniff 112233445566 0",
      "sniff 112233445566 14", "sniff 112233445566 -1", "sniff 112233445566 6 extra"};
  for (const char *command : invalidCommands)
    assert(UserCommand::parse(command).kind == Kind::Invalid);
  UserCommand::LineReader reader;
  using InputResult = UserCommand::LineReader::Result;
  for (char value : std::array<char, 4>{'c', 'o', 'n', 'f'})
    assert(reader.push(value, false) == InputResult::None);
  assert(reader.push('\r', false) == InputResult::Line);
  assert(strcmp(reader.text(), "conf") == 0);
  assert(reader.push('\n', true) == InputResult::None);  // CRLF doesn't queue an extra command.
  assert(reader.push('c', true) == InputResult::None);
  reader.push('o', false); reader.push('n', false); reader.push('f', false);
  assert(reader.push('\n', false) == InputResult::Busy);  // Started while busy, ended later.
  for (unsigned i = 0; i < 80; ++i) reader.push('x', false);
  assert(reader.push('\n', false) == InputResult::Invalid);
  for (const char *text = "conf"; *text; ++text) reader.push(*text, false);
  assert(reader.push('\n', false) == InputResult::Line);  // Overflow doesn't poison next command.
  reader.push('c', false);
  assert(reader.push('\n', true) == InputResult::Busy);  // Became busy halfway through input.
  // Known path-loss reference points and invalid calibration/measurements.
  assert(A == -60.0 && n == 2.0);
  assert(std::fabs(rssiToDistance(-60, A, n) - 1.0) < 1e-10);
  assert(std::fabs(rssiToDistance(-80, A, n) - 10.0) < 1e-10);
  assert(std::fabs(rssiToDistance(-40, A, n) - 0.1) < 1e-10);
  assert(std::fabs(rssiToDistance(-80, -60, 4) - std::sqrt(10.0)) < 1e-10);
  assert(std::isnan(rssiToDistance(-60, A, 0)));
  assert(std::isnan(rssiToDistance(-60, A, -2)));
  assert(std::isnan(rssiToDistance(std::numeric_limits<double>::quiet_NaN(), A, n)));
  assert(std::isnan(rssiToDistance(-60, std::numeric_limits<double>::infinity(), n)));
  assert(std::isnan(rssiToDistance(-10000, A, 0.001)));  // Overflow.
  using namespace TimeSliceProtocol;
  assert(sniffDurationUs(true) == 10000000);
  assert(sniffDurationUs(false) == 20000000);
  assert(nextSniffCycle(0) == 1);
  assert(nextSniffCycle(1) == 2);
  assert(nextSniffCycle(2) == 3);
  assert(nextSniffCycle(3) == 0);  // No fourth cycle.
  DistanceAverage average;
  assert(std::isnan(average.mean()));
  average.add(1.0); average.add(3.0); average.add(5.0);
  assert(average.count == 3 && std::fabs(average.mean() - 3.0) < 1e-10);
  DistanceAverage partial;
  partial.add(2.0); partial.add(std::numeric_limits<double>::quiet_NaN()); partial.add(4.0);
  assert(partial.count == 2 && std::fabs(partial.mean() - 3.0) < 1e-10);
  partial.add(0); partial.add(-1); partial.add(std::numeric_limits<double>::infinity());
  assert(partial.count == 2);
  assert(sniffChannel(-1, 1, 11) == 1);
  assert(sniffChannel(CHANNEL_DWELL_US - 1, 1, 11) == 1);
  assert(sniffChannel(CHANNEL_DWELL_US, 1, 11) == 2);
  assert(sniffChannel(11 * CHANNEL_DWELL_US, 1, 11) == 1);
  assert(sniffChannel(12 * CHANNEL_DWELL_US, 1, 13) == 13);
  assert(sniffChannel(0, 5, 4) == 5);
  uint8_t targetPacket[40] = {};
  const uint8_t targetMac[6] = {0x44, 0x33, 0x22, 0x11, 0, 2};
  memcpy(targetPacket + 10, targetMac, 6);
  assert(targetFrame(targetPacket, sizeof(targetPacket), targetMac));  // Management.
  for (size_t length = 0; length < 28; ++length)
    assert(!targetFrame(targetPacket, length, targetMac));
  targetPacket[0] = 0x08;  // Data, source at address 2 regardless of DS flags.
  for (uint8_t ds = 0; ds < 4; ++ds) {
    targetPacket[1] = ds;
    assert(targetFrame(targetPacket, sizeof(targetPacket), targetMac));
  }
  assert(!targetFrame(targetPacket, 28, targetMac));  // Short four-address header.
  targetPacket[0] = 0x04;  // Control frames are excluded.
  assert(!targetFrame(targetPacket, sizeof(targetPacket), targetMac));
  targetPacket[0] = 0x08;
  targetPacket[10]++;
  memcpy(targetPacket + 16, targetMac, 6);
  assert(!targetFrame(targetPacket, sizeof(targetPacket), targetMac));  // Relay source != transmitter.
  // Boards may have different uptimes. They must derive deadlines for the
  // same shared mesh instant, including when the 32-bit mesh timer wraps.
  assert(4000000LL + remainingUs(20000000, 10200000) == 13800000LL);
  const uint32_t beforeWrap = 0xFFFFFF00u;
  assert(remainingUs(beforeWrap + START_LEAD_US, beforeWrap) == START_LEAD_US);
  assert(remainingUs(1000000, 2000000) < 0);

  const int64_t start = 100000000;
  size_t slot = 999;
  assert(!activeSlot(start - 1, start, 4, slot));
  assert(!activeSlot(start + GUARD_US - 1, start, 4, slot));
  assert(activeSlot(start + GUARD_US, start, 4, slot) && slot == 0);
  assert(activeSlot(start + SLOT_US - GUARD_US - 1, start, 4, slot));
  assert(!activeSlot(start + SLOT_US - GUARD_US, start, 4, slot));
  assert(!activeSlot(start + SLOT_US, start, 4, slot));
  assert(activeSlot(start + SLOT_US + GUARD_US, start, 4, slot) && slot == 1);
  assert(!activeSlot(start + 4 * SLOT_US, start, 4, slot));
  assert(activeSlot(start + 99LL * SLOT_US + GUARD_US, start, 100, slot));
  assert(slot == 99);
  assert(SLOT_US - 2 * GUARD_US == SAMPLE_US);
  assert(sampleDurationUs(false) == 3000000 && slotDurationUs(false) == 4000000);
  assert(sampleDurationUs(true) == 10000000 && slotDurationUs(true) == 11000000);
  assert(CALIBRATION_MIN_SAMPLES == 300);
  const uint32_t calibrationSlot = slotDurationUs(true);
  assert(!activeSlot(start + GUARD_US - 1, start, 2, slot, true));
  assert(activeSlot(start + GUARD_US, start, 2, slot, true) && slot == 0);
  // Calibration continues collecting where an ordinary conf slot has ended.
  assert(activeSlot(start + 9000000, start, 2, slot, true) && slot == 0);
  assert(activeSlot(start + calibrationSlot - GUARD_US - 1, start, 2, slot, true) && slot == 0);
  assert(!activeSlot(start + calibrationSlot - GUARD_US, start, 2, slot, true));
  assert(activeSlot(start + calibrationSlot + GUARD_US, start, 2, slot, true) && slot == 1);
  assert(!activeSlot(start + 2LL * calibrationSlot, start, 2, slot, true));
  WindowTiming timing;
  const int64_t opening = start + GUARD_US, closing = opening + CALIBRATION_SAMPLE_US;
  timing.observe(opening - 1, opening, closing);
  assert(timing.openedUs < 0 && timing.elapsedMs() == 0);
  timing.observe(opening + 1000, opening, closing);
  timing.observe(closing - 1, opening, closing);
  assert(timing.closedUs < 0);
  timing.observe(closing + 2000, opening, closing);
  assert(timing.elapsedMs() == 10001); // Observed time includes main-loop boundary jitter.
  timing.observe(closing + 1000000, opening, closing);
  assert(timing.elapsedMs() == 10001); // Closing is recorded once.
  WindowTiming missed;
  missed.observe(closing + 1, opening, closing);
  assert(missed.elapsedMs() == 0 && missed.openedUs < 0); // Never invent a full capture.
  assert(measurementActivity(start - 1, start, 2, 0, true) == RadioActivity::Waiting);
  assert(measurementActivity(start + GUARD_US, start, 2, 0, true) == RadioActivity::Transmit);
  assert(measurementActivity(start + GUARD_US, start, 2, 1, true) == RadioActivity::Receive);
  assert(measurementActivity(start + calibrationSlot - GUARD_US, start, 2, 0, true) == RadioActivity::Waiting);
  assert(measurementActivity(start + calibrationSlot + GUARD_US, start, 2, 0, true) == RadioActivity::Receive);
  assert(measurementActivity(start + calibrationSlot + GUARD_US, start, 2, 1, true) == RadioActivity::Transmit);
  assert(measurementActivity(start + 2LL * calibrationSlot, start, 2, 0, true) == RadioActivity::Waiting);
  assert(measurementActivity(start + SLOT_US + GUARD_US, start, 2, 1) == RadioActivity::Transmit);

  uint8_t frame[39 + PAYLOAD_SIZE + 4] = {};
  frame[0] = 0xD0;
  frame[24] = 127;
  frame[25] = frame[34] = 0x18;
  frame[26] = frame[35] = 0xFE;
  frame[27] = frame[36] = 0x34;
  frame[32] = 221;
  frame[33] = 5 + PAYLOAD_SIZE;
  frame[37] = 4;
  frame[38] = 1;
  makePayload(frame + 39, 0xABCDEF01u, 0x87654321u, 513, 29);
  const uint8_t *payload = measurementPayload(frame, sizeof(frame));
  assert(payload == frame + 39);
  assert(read32(payload + 4) == 0xABCDEF01u);
  assert(read32(payload + 8) == 0x87654321u);
  assert(((unsigned(payload[12]) << 8) | payload[13]) == 513);
  assert(read32(payload + 14) == 29);

  const uint8_t source[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  uint8_t otherSource[6];
  memcpy(otherSource, source, 6);
  otherSource[5]++;
  assert(matchesMeasurement(payload, source, source, 0xABCDEF01u,
                            0x87654321u, 513, 28));
  assert(!matchesMeasurement(payload, source, source, 0xABCDEF02u,
                             0x87654321u, 513, 28));  // Stale/wrong round.
  assert(!matchesMeasurement(payload, source, source, 0xABCDEF01u,
                             0x87654322u, 513, 28));  // Different table order.
  assert(!matchesMeasurement(payload, source, source, 0xABCDEF01u,
                             0x87654321u, 514, 28));  // Wrong slot.
  assert(!matchesMeasurement(payload, source, otherSource, 0xABCDEF01u,
                             0x87654321u, 513, 28));  // Wrong transmitter.
  assert(!matchesMeasurement(payload, source, source, 0xABCDEF01u,
                             0x87654321u, 513, 29));  // Duplicate packet.

  // Truncated frames must be rejected before reading payload or header data.
  for (size_t length = 0; length < sizeof(frame); ++length) {
    assert(measurementPayload(frame, length) == nullptr);
  }
  frame[38] = 2;
  assert(measurementPayload(frame, sizeof(frame)) != nullptr);
  frame[38] |= 0x10;  // Fragmented/more-data ESP-NOW frame.
  assert(measurementPayload(frame, sizeof(frame)) == nullptr);
  frame[38] = 2;
  frame[39] = 'X';
  assert(measurementPayload(frame, sizeof(frame)) == nullptr);
  frame[39] = 'R';
  frame[33]--;
  assert(measurementPayload(frame, sizeof(frame)) == nullptr);
  frame[33]++;
  frame[34] = 0;  // Unrelated vendor frame.
  assert(measurementPayload(frame, sizeof(frame)) == nullptr);
  puts("Commands, channel hopping, target frames, schedule and RSSI-distance tests passed");
}
