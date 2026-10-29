// Host test: add tests/stubs and the installed ArduinoJson/src to include paths.
#include <ArduinoJson.h>
#include <array>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include "../shared/rssi_distance.h"

#define BOARD_NUMBER 2
class String : public std::string {
 public:
  using std::string::string;
  void trim() {
    const auto first = find_first_not_of(" \t\r\n");
    if (first == npos) { clear(); return; }
    const auto last = find_last_not_of(" \t\r\n");
    assign(substr(first, last - first + 1));
  }
  size_t write(uint8_t byte) { push_back(char(byte)); return 1; }
  size_t write(const uint8_t *bytes, size_t size) {
    append(reinterpret_cast<const char *>(bytes), size); return size;
  }
};
struct TestSerial {
  std::string output;
  void println(const char *text = "") { output += text; output += '\n'; }
  size_t write(uint8_t byte) { output.push_back(char(byte)); return 1; }
  size_t write(const uint8_t *bytes, size_t size) {
    output.append(reinterpret_cast<const char *>(bytes), size); return size;
  }
  void print(const char *text) { output += text; }
  template<class... Args> void printf(const char *format, Args... args) {
    char buffer[512]; std::snprintf(buffer, sizeof(buffer), format, args...); output += buffer;
  }
} Serial;
uint32_t ticks = 1000;
uint32_t millis() { return ticks; }
uint32_t esp_random() { static uint32_t id = 100; return ++id; }
namespace painlessmesh { namespace tcp {
uint32_t encodeNodeId(const uint8_t *ap) { return ap[5]; }
} }
using MacAddress = std::array<uint8_t, 6>;
std::vector<MacAddress> macList{{0x44,0x33,0x22,0x11,0,1}, {0x44,0x33,0x22,0x11,0,2}};
uint8_t AP_self_mac[6] = {0x54,0x44,0x33,0x22,0x11,2};
bool meshStarted = true;
struct TestMesh {
  bool connected = true;
  unsigned broadcasts = 0, singles = 0;
  String lastBroadcast;
  std::vector<uint32_t> getNodeList() const { return connected ? std::vector<uint32_t>{2} : std::vector<uint32_t>{}; }
  bool isConnected(uint32_t) const { return connected; }
  void sendBroadcast(const String &s) { lastBroadcast = s; ++broadcasts; }
  void sendSingle(uint32_t, const String &) { ++singles; }
} mesh;
struct TimeSlicingController {
  enum class Phase { IDLE, FAILED };
  bool root = true, running = false;
  uint32_t round = 0;
  bool isRoot() const { return root; }
  uint32_t rootNodeId() const { return 1; }
  bool busy() const { return running; }
  Phase phase() const { return Phase::IDLE; }
  bool networkReadyForConfiguration(size_t) const { return !running && meshStarted; }
  bool beginCalibrationRound(const std::vector<MacAddress> &pair, uint32_t session) {
    assert(pair.size() == 2 && session); ++round; running = true; return true;
  }
  uint32_t roundId() const { return round; }
} timeSlicing;
double A = -60, n = 2;
JsonDocument roundReports;
#include "../shared/calibration_mode.h"

void acknowledge(CalibrationMode &mode, bool identified = true) {
  JsonDocument broadcast;
  assert(!deserializeJson(broadcast, mesh.lastBroadcast.c_str()));
  JsonDocument doc;
  doc["Action"] = "hello"; doc["Board"] = 2;
  doc["Revision"] = broadcast["Revision"].as<uint32_t>();
  doc["Session"] = mode.session(); doc["Identified"] = identified;
  doc["SyncEpoch"] = broadcast["SyncEpoch"].as<uint32_t>();
  auto ap = doc["AP"].to<JsonArray>();
  for (uint8_t byte : AP_self_mac) ap.add(byte);
  assert(mode.handle(doc, 2));
}
void completeCapture(double distance, double a = -50, double exponent = 2, double spread = 1) {
  assert(timeSlicing.running);
  roundReports.clear();
  for (unsigned reporter = 1; reporter <= 2; ++reporter) {
    const char *report = reporter == 1 ? "1" : "2";
    const char *target = reporter == 1 ? "2" : "1";
    roundReports[report]["RoundId"] = timeSlicing.round;
    roundReports[report]["Status"] = "ok";
    roundReports[report]["Samples"][target] = 500;
    roundReports[report]["SampleWindowMs"] = 10000;
    roundReports[report]["RadioElapsedMs"] = 22000;
    roundReports[report]["ObservedWindowMs"][target] = 10000;
    roundReports[report]["PacketSpanMs"][target] = 9980;
    roundReports[report]["FilteredRSSI"][target] = a - 10 * exponent * std::log10(distance);
    roundReports[report]["RSSISpread"][target] = spread;
    roundReports[report]["RadioConfig"]["ReadOK"] = true;
    roundReports[report]["RadioConfig"]["WiFiChannel"] = 1;
    roundReports[report]["RadioConfig"]["TxPowerCapQuarterDbm"] = 60;
    roundReports[report]["ReceivedChannel"][target] = 1;
    roundReports[report]["ChannelChanged"][target] = false;
  }
  timeSlicing.running = false;
}
void startCapture(CalibrationMode &mode) {
  acknowledge(mode);
  mode.command("next"); ticks += 2001; mode.update();
  assert(timeSlicing.running);
}

int main() {
  CalibrationMode mode;
  mode.begin(); mode.update(); assert(mesh.broadcasts == 0); // No boot timer broadcast.
  mode.topologyChanged(); mode.update(); acknowledge(mode); mode.update();
  assert(mode.networkSynced());
  const unsigned idleBroadcasts = mesh.broadcasts;
  ticks += 60000; mode.update();
  assert(mesh.broadcasts == idleBroadcasts && mode.networkSynced()); // No stale heartbeat requirement.
  mode.topologyChanged(); assert(!mode.networkSynced());
  mode.update(); assert(mesh.broadcasts == idleBroadcasts + 1);
  ticks += 1001; mode.update(); assert(mesh.broadcasts == idleBroadcasts + 2); // Retry lost ACK.
  for (unsigned i = 0; i < 10; ++i) { ticks += 1001; mode.update(); }
  assert(mesh.broadcasts == idleBroadcasts + 5); // Idle retries stop after five attempts.
  acknowledge(mode); mode.update();
  assert(mode.start(2)); assert(mode.identifying());
  mode.command("next"); assert(!timeSlicing.running); // No capture during identification.
  ticks += 7001; mode.update(); acknowledge(mode); mode.update();
  assert(!mode.identifying());
  const unsigned waitingBroadcasts = mesh.broadcasts;
  for (unsigned i = 0; i < 600; ++i) { ticks += 1001; mode.update(); }
  assert(!timeSlicing.running && mesh.broadcasts == waitingBroadcasts); // Silent ten-minute placement wait.
  for (unsigned point = 1; point <= 5; ++point) {
    startCapture(mode);
    completeCapture(point); mode.update();
    assert(A == -60 && n == 2); // Fits remain provisional.
  }
  mode.command("save"); assert(A == -60); // Validation required.
  startCapture(mode); completeCapture(2.5); mode.update();
  testNvsWriteFailure = true;
  mode.command("save"); assert(A == -60 && testNvs.empty());
  testNvsWriteFailure = false;
  mode.command("save"); assert(std::fabs(A + 50) < 1e-9 && !testNvs.empty());
  mode.update(); assert(mode.active()); // Old peer revision cannot finish save.
  mesh.connected = false; ticks += 31000; mode.update();
  assert(mode.active() && std::fabs(A + 50) < 1e-9); // Save survives a disconnected node.
  mesh.connected = true;
  acknowledge(mode); mode.update(); assert(!mode.active());
  A = -60; n = 2;
  CalibrationMode reboot; reboot.begin(); assert(std::fabs(A + 50) < 1e-9);
  reboot.topologyChanged(); ticks += 1001; reboot.update(); acknowledge(reboot);
  assert(reboot.start(2));
  ticks += 7001; reboot.update(); acknowledge(reboot); reboot.update();
  startCapture(reboot);
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    completeCapture(1, -50, 2, attempt < 2 ? 1 : 7);
    if (attempt == 0) roundReports["1"]["Samples"]["2"] = 299;
    if (attempt == 1) roundReports["2"]["SampleWindowMs"] = 3000; // Reject old calibration timing.
    reboot.update();
    assert(!timeSlicing.running);
    if (attempt < 2) { ticks += 2001; acknowledge(reboot); reboot.update(); assert(timeSlicing.running); }
  }
  ticks += 10000; reboot.update(); assert(!timeSlicing.running); // Bounded automatic retries.
  startCapture(reboot);
  // The threshold is inclusive: exactly 300 samples is sufficient.
  completeCapture(1);
  roundReports["1"]["Samples"]["2"] = 300;
  roundReports["2"]["Samples"]["1"] = 300;
  reboot.update();
  assert(Serial.output.find("observed window=10.000 s") != std::string::npos);
  assert(Serial.output.find("first-to-last packet span=9.980 s") != std::string::npos);
  startCapture(reboot);
  reboot.command("cancel"); assert(reboot.active()); // Defer until mesh restored.
  completeCapture(1); reboot.update();
  assert(!reboot.active() && std::fabs(A + 50) < 1e-9);

  // Replay the supplied field data: a low-noise but inconsistent 5 m point
  // must block saving, and can be replaced without repeating 1..4 m.
  CalibrationMode observed;
  observed.begin(); A = -60; n = 2;
  observed.topologyChanged(); observed.update(); acknowledge(observed);
  assert(observed.start(2));
  ticks += 7001; observed.update(); acknowledge(observed); observed.update();
  const double fieldForward[] = {-59.69,-62.97,-68.91,-73.61,-83.42};
  const double fieldReverse[] = {-59.41,-63.30,-71.12,-71.76,-82.50};
  const auto savedBeforeField = testNvs;
  for (unsigned i = 0; i < 5; ++i) {
    startCapture(observed); completeCapture(i + 1);
    roundReports["1"]["FilteredRSSI"]["2"] = fieldForward[i];
    roundReports["2"]["FilteredRSSI"]["1"] = fieldReverse[i];
    observed.update();
  }
  assert(Serial.output.find("RECHECK (>30% error)") != std::string::npos);
  observed.command("save"); assert(A == -60 && n == 2 && testNvs == savedBeforeField);
  observed.command("remeasure 6"); // Invalid input cannot start capture.
  assert(!timeSlicing.running);
  observed.command("remeasure 5"); Serial.output.clear();
  startCapture(observed); completeCapture(5);
  roundReports["1"]["FilteredRSSI"]["2"] = -75.1;
  roundReports["2"]["FilteredRSSI"]["1"] = -75.1;
  observed.update();
  assert(Serial.output.find("Validation: Place selected boards 2.5") != std::string::npos);
  observed.command("save"); assert(A == -60 && n == 2 && testNvs == savedBeforeField);
  observed.command("cancel"); assert(!observed.active() && testNvs == savedBeforeField);

  // A clean training fit is still rejected when held-out validation gets
  // worse than the current model, even though it meets the old 30% gate.
  CalibrationMode regression;
  regression.begin(); A = -60; n = 2;
  regression.topologyChanged(); regression.update(); acknowledge(regression);
  assert(regression.start(2));
  ticks += 7001; regression.update(); acknowledge(regression); regression.update();
  for (unsigned i = 1; i <= 5; ++i) {
    startCapture(regression); completeCapture(i, -58, 2); regression.update();
  }
  startCapture(regression); completeCapture(2.5, -60, 2); regression.update();
  assert(Serial.output.find("candidate error exceeds previous model") != std::string::npos);
  regression.command("save"); assert(A == -60 && n == 2 && testNvs == savedBeforeField);
  regression.command("cancel");
  A = -50; n = 2; // Restore the persisted model for node synchronization cases.

  // A node accepts only root state, acknowledges only after persistence,
  // and does not overwrite globals during an active radio round.
  // Fixed-position diagnostics run five windows after one next, with no model write.
  CalibrationMode diagnostic; diagnostic.begin();
  diagnostic.topologyChanged(); diagnostic.update(); acknowledge(diagnostic);
  const auto savedBeforeDiagnostic = testNvs;
  const double oldA = A, oldN = n;
  assert(diagnostic.start(2, true, 2));
  ticks += 7001; diagnostic.update(); acknowledge(diagnostic); diagnostic.update();
  diagnostic.command("save");
  assert(!timeSlicing.running && testNvs == savedBeforeDiagnostic);
  startCapture(diagnostic);
  for (unsigned window = 0; window < 5; ++window) {
    completeCapture(2, -70 + double(window), 2, window == 1 ? 7 : 1);
    diagnostic.update();
    assert(A == oldA && n == oldN && testNvs == savedBeforeDiagnostic);
    if (window < 4) {
      ticks += 2001; acknowledge(diagnostic); diagnostic.update();
      assert(timeSlicing.running); // No additional next command.
    }
  }
  assert(!diagnostic.active() && !timeSlicing.running);
  assert(Serial.output.find("valid=4/5") != std::string::npos);
  assert(Serial.output.find("A/n and saved calibration unchanged") != std::string::npos);
  timeSlicing.root = false;
  CalibrationMode node; node.begin();
  unsigned singles = mesh.singles;
  ticks += 10000; node.update(); assert(mesh.singles == singles); // Idle node sends no timer hello.
  node.topologyChanged(); node.update(); assert(mesh.singles == singles + 1);
  JsonDocument state;
  state["Action"] = "state"; state["Session"] = uint32_t(42); state["Selected"] = 2;
  state["Revision"] = uint32_t(777); state["A"] = -48.0; state["n"] = 2.4;
  state["Persist"] = true;
  assert(!node.handle(state, 999));
  timeSlicing.running = true;
  assert(!node.handle(state, 1)); assert(std::fabs(A + 50) < 1e-9);
  timeSlicing.running = false;
  testNvsWriteFailure = true;
  assert(!node.handle(state, 1)); assert(std::fabs(A + 50) < 1e-9);
  testNvsWriteFailure = false;
  assert(node.handle(state, 1)); assert(A == -48 && n == 2.4 && node.identifying());
  JsonDocument configuration;
  configuration["Calibration"] = true; configuration["CalibrationSession"] = uint32_t(41);
  assert(!node.allowConfiguration(configuration));
  configuration["CalibrationSession"] = uint32_t(42);
  assert(node.allowConfiguration(configuration));
  ticks += 7001; node.update(); // One identification-complete event, bounded retries.
  singles = mesh.singles;
  for (unsigned i = 0; i < 10; ++i) { ticks += 1001; node.update(); }
  assert(mesh.singles == singles + 4);
  singles = mesh.singles;
  for (unsigned i = 0; i < 600; ++i) { ticks += 1001; node.update(); }
  assert(node.active() && mesh.singles == singles); // Connected placement wait never expires or sends heartbeats.
  mesh.connected = false;
  ticks += 180001; node.update(); assert(!node.active());
  singles = mesh.singles;
  ticks += 10000; node.update(); assert(mesh.singles == singles);
  std::puts("Calibration placement, validation, save, cancel, reboot and root-sync lifecycle passed");
}
