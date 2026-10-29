#pragma once

#include <Preferences.h>
#include <cstring>
#include "calibration_math.h"
#include "../root_node/time_slicing_protocol.h"

// Included after the mesh, controller, A and n globals in node_firmware.h.
// All radio capture remains in TimeSlicingController; this class orchestrates
// placement, identification, fitting, NVS persistence and model distribution.
class CalibrationMode {
 public:
  bool active() const { return state_ != State::Idle || remoteSession_ != 0; }
  bool localSession() const { return state_ != State::Idle; }
  // Queue work from mesh callbacks; send only later from loop(). Idle peers
  // do not need heartbeats once this connection event is acknowledged.
  void topologyChanged() {
    syncPending_ = true;
    syncAttempts_ = 0;
    lastSend_ = millis() - 1000;
    if (timeSlicing.isRoot()) syncEpoch_ = newId();
  }
  bool identifying() const {
    return (localSession() && state_ == State::Identify && millis() - identifySince_ < 7000) ||
        (remoteSession_ && selected_ == BOARD_NUMBER && millis() - identifySince_ < 7000);
  }
  void begin() {
    Blob stored{};
    Preferences prefs;
    if (prefs.begin("rssi-model", true)) {
      if (prefs.getBytesLength("model") == sizeof(stored))
        prefs.getBytes("model", &stored, sizeof(stored));
      prefs.end();
    }
    if (validBlob(stored)) {
      A = stored.a; n = stored.exponent; revision_ = stored.revision; persisted_ = true;
      Serial.printf("[AN] Loaded saved globals: A=%.4f n=%.4f\n", A, n);
    }
    if (timeSlicing.isRoot() && !revision_) revision_ = newId();
  }

  bool start(uint8_t board, bool diagnostic = false, uint8_t fixedDistance = 1) {
    if (!timeSlicing.isRoot() || active() || timeSlicing.busy() ||
        board < 2 || board > macList.size() || !networkSynced() || fixedDistance < 1 || fixedDistance > 5) {
      Serial.println("[AN] Wait for all configured boards to connect and acknowledge the model");
      return false;
    }
    selected_ = board; session_ = newId(); point_ = attempt_ = 0;
    diagnostic_ = diagnostic; diagnosticDistance_ = fixedDistance; diagnosticRound_ = 0;
    diagnosticForward_.fill(NAN); diagnosticReverse_.fill(NAN);
    diagnosticForwardValid_.fill(false); diagnosticReverseValid_.fill(false);
    baselineA_ = A; baselineN_ = n; remeasuring_ = acceptable_ = false;
    abort_ = false; state_ = State::Identify; identifySince_ = millis();
    stateSince_ = millis(); lastSend_ = millis() - 1000;
    syncPending_ = true; syncAttempts_ = 0;
    for (auto &peer : peers_) { peer.lock = 0; peer.identified = false; }
    Serial.printf("[AN] Root 01 and node %02u: blink for 7 seconds, then wait for placement\n", board);
    if (diagnostic_) Serial.println("[DIAG] Fixed-position diagnostic: five captures; A/n and saved calibration will not be changed");
    return true;
  }

  // Calibration input is intentionally independent of the round busy guard.
  void command(const char *raw) {
    String text(raw); text.trim();
    if (diagnostic_ && text != "next" && text != "cancel") {
      Serial.println("[DIAG] Use next at the placement prompt, or cancel. Diagnostics cannot save calibration");
      return;
    }
    if (text == "cancel") {
      if (state_ == State::Save) {
        Serial.println("[AN] Save already committed; waiting for distribution. Cannot undo with cancel");
        return;
      }
      abort_ = true;
      if (timeSlicing.busy()) Serial.println("[AN] Cancel requested; restoring mesh after this capture");
      else finish(false);
      return;
    }
    if (text == "next" && state_ == State::Wait) {
      state_ = State::Settle; stateSince_ = millis(); attempt_ = 0;
      stepSinceMs_ = millis();
      Serial.println("[AN] Settling for 2 seconds; keep boards stationary");
    } else if (text == "retry" && state_ == State::Review) {
      point_ = attempt_ = 0; remeasuring_ = acceptable_ = false; prompt();
    } else if (state_ == State::Review && std::strncmp(text.c_str(), "remeasure ", 10) == 0) {
      const char *distance = text.c_str() + 10;
      while (*distance == ' ' || *distance == '\t') ++distance;
      if (*distance < '1' || *distance > '5' || distance[1]) {
        Serial.println("[AN] Use remeasure <distance 1..5>");
        return;
      }
      point_ = uint8_t(*distance - '1'); attempt_ = 0;
      remeasuring_ = true; acceptable_ = false;
      Serial.println("[AN] Reposition the pair and check antenna orientation/path; other distance readings are retained");
      prompt();
    } else if (text == "save" && state_ == State::Review && acceptable_) {
      // Commit one blob before changing the active globals. A failed write
      // leaves the previous runtime model untouched.
      const uint32_t revision = newId();
      if (!store(candidate_.a, candidate_.n, revision)) {
        Serial.println("[AN] NVS write failed; old A/n remain active. Retry save or cancel");
        return;
      }
      A = candidate_.a; n = candidate_.n; revision_ = revision; persisted_ = true;
      state_ = State::Save; stateSince_ = millis(); lastSend_ = millis() - 1000;
      syncPending_ = true; syncAttempts_ = 0;
      Serial.printf("[AN] Saved and updated root globals: A=%.4f n=%.4f; waiting for node acknowledgements\n", A, n);
    } else {
      Serial.println(state_ == State::Review
          ? (acceptable_ ? "[AN] Use save | remeasure <1..5> | retry | cancel"
                         : "[AN] Save blocked: use remeasure <1..5> | retry | cancel")
          : "[AN] Use next when prompted, or cancel; conf/sniff are unavailable during calibration");
    }
  }

  bool handle(JsonDocument &doc, uint32_t from) {
    const char *action = doc["Action"] | "";
    if (strcmp(action, "hello") == 0 && timeSlicing.isRoot()) {
      if (!doc["Board"].is<unsigned>() || !doc["AP"].is<JsonArray>()) return false;
      const unsigned board = doc["Board"];
      if (board < 2 || board > macList.size() || doc["AP"].size() != 6) return false;
      uint8_t ap[6];
      for (unsigned i = 0; i < 6; ++i) {
        if (!doc["AP"][i].is<unsigned>() || doc["AP"][i].as<unsigned>() > 255) return false;
        ap[i] = doc["AP"][i].as<unsigned>();
      }
      if (painlessmesh::tcp::encodeNodeId(ap) != from || !mesh.isConnected(from)) return false;
      Peer *peer = nullptr;
      for (auto &entry : peers_) {
        if (entry.board != board && entry.id == from) return false;
        if (entry.board == board) peer = &entry;
      }
      if (!peer) { peers_.push_back(Peer{}); peer = &peers_.back(); }
      if (peer->id && peer->id != from && mesh.isConnected(peer->id)) {
        Serial.println("[AN] Duplicate board number; check BOARD_NUMBER on uploaded nodes");
        return false;
      }
      peer->board = board; peer->id = from; peer->seen = millis();
      peer->revision = doc["Revision"] | uint32_t(0);
      peer->lock = doc["Session"] | uint32_t(0);
      peer->identified = doc["Identified"] | false;
      peer->sync = doc["SyncEpoch"] | uint32_t(0);
      return true;
    }
    if (timeSlicing.isRoot() || from != timeSlicing.rootNodeId()) return false;
    if (strcmp(action, "state") != 0 || !doc["Session"].is<uint32_t>() ||
        !doc["Revision"].is<uint32_t>() || !doc["A"].is<double>() ||
        !doc["n"].is<double>() || !doc["Persist"].is<bool>()) return false;
    const uint32_t session = doc["Session"];
    const unsigned selected = doc["Selected"] | 0u;
    const uint32_t revision = doc["Revision"];
    const double a = doc["A"], exponent = doc["n"];
    if (!revision || !validParameters(a, exponent) ||
        (session && (selected < 2 || selected > 15))) return false;
    if (timeSlicing.busy()) return false;  // Never change a running round's globals.
    if ((revision != revision_ || A != a || n != exponent) ||
        (doc["Persist"].as<bool>() && !persisted_)) {
      if (doc["Persist"].as<bool>() && !store(a, exponent, revision)) {
        Serial.println("[AN] Cannot persist root model; withholding acknowledgement");
        return false;
      }
      A = a; n = exponent; revision_ = revision; persisted_ = doc["Persist"].as<bool>();
      Serial.printf("[AN] Updated globals from root: A=%.4f n=%.4f%s\n", A, n,
                    persisted_ ? " (saved)" : "");
    }
    if (session != remoteSession_) {
      remoteSession_ = session; selected_ = selected; identifySince_ = millis();
      identificationAnnounced_ = false;
      if (session && selected_ == BOARD_NUMBER)
        Serial.println("[AN] Selected with root: identifying for 7 seconds");
    }
    remoteSeen_ = millis();
    remoteSyncEpoch_ = doc["SyncEpoch"] | uint32_t(0);
    syncPending_ = false;
    hello();
    return true;
  }

  // Only selected nodes accept the pair's conf packets. Bystanders retain
  // their full station list and stay in mesh, with no ESP-NOW transmissions.
  bool allowConfiguration(JsonDocument &doc) const {
    const bool calibration = doc["Calibration"] | false;
    if (active()) return !timeSlicing.isRoot() && remoteSession_ &&
        selected_ == BOARD_NUMBER && calibration &&
        doc["CalibrationSession"].as<uint32_t>() == remoteSession_;
    return !calibration;
  }
  uint32_t session() const { return timeSlicing.isRoot() ? session_ : remoteSession_; }
  bool networkSynced() const {
    if (!meshStarted) return false;
    for (unsigned board = 2; board <= macList.size(); ++board) {
      const Peer *peer = findPeer(board);
      if (!peer || !fresh(*peer) || peer->revision != revision_ || peer->sync != syncEpoch_) return false;
    }
    return true;
  }

  void update() {
    const uint32_t now = millis();
    // Identify completion is a single event, not a periodic session heartbeat.
    if (!timeSlicing.isRoot() && remoteSession_ && selected_ == BOARD_NUMBER &&
        !identificationAnnounced_ && now - identifySince_ >= 7000) {
      identificationAnnounced_ = true;
      syncPending_ = true; syncAttempts_ = 0; lastSend_ = now - 1000;
    }
    if (timeSlicing.isRoot() && syncPending_ && connectedPeersSynced()) syncPending_ = false;
    if (meshStarted && !timeSlicing.busy() && syncPending_ &&
        now - lastSend_ >= 1000) {
      lastSend_ = now;
      if (timeSlicing.isRoot()) broadcastState(); else hello();
      // Every event has bounded delivery retries, including calibration.
      if (++syncAttempts_ >= 5) syncPending_ = false;
    }
    if (!timeSlicing.isRoot()) {
      // Mesh connectivity keeps placement waits alive without radio messages.
      // Only release a lock after the root has actually been unreachable.
      if (remoteSession_ && meshStarted && mesh.isConnected(timeSlicing.rootNodeId()))
        remoteSeen_ = now;
      if (remoteSession_ && !timeSlicing.busy() && now - remoteSeen_ > 180000) {
        remoteSession_ = 0;
        Serial.println("[AN] Root unreachable for 180 seconds; calibration lock released");
      }
      return;
    }
    if (!localSession()) return;
    if (abort_ && !timeSlicing.busy()) { finish(false); return; }
    if (state_ == State::Identify) {
      const Peer *selected = findPeer(selected_);
      if (now - identifySince_ >= 7000 && allLocked() && selected && selected->identified) prompt();
      else if (now - stateSince_ > 30000) {
        Serial.println("[AN] Identification timed out: not all nodes acknowledged the lock"); finish(false);
      }
    } else if (state_ == State::Settle) {
      if (now - stateSince_ < 2000) return;
      const Peer *peer = findPeer(selected_);
      if (!allLocked() || !peer || !timeSlicing.networkReadyForConfiguration(2)) {
        if (now - stateSince_ > 30000) {
          Serial.println("[AN] Mesh not ready; enter next to retry this distance");
          state_ = State::Wait;
        }
        return;
      }
      std::vector<MacAddress> pair{macList[0], macList[selected_ - 1]};
      if (timeSlicing.beginCalibrationRound(pair, session_)) {
        round_ = timeSlicing.roundId(); state_ = State::Capture;
        roundSinceMs_ = millis();
        ++attempt_;
        if (diagnostic_)
          Serial.printf("[DIAG] Fixed %.1f m: window %u/5. Keep BOTH boards, antennas and cables fixed\n",
                        distance(), unsigned(diagnosticRound_ + 1));
        else Serial.printf("[AN] %.1f m, capture %u/3; 10 seconds per direction, minimum 300 samples each\n", distance(), attempt_);
      }
    } else if (state_ == State::Capture) {
      if (timeSlicing.busy()) return;
      if (timeSlicing.phase() == TimeSlicingController::Phase::FAILED) {
        if (now - stateSince_ >= 5000) {
          Serial.println("[AN] Radio recovery pending; cancel or wait for mesh recovery");
          stateSince_ = now;
        }
        return;
      }
      readCapture();
    } else if (state_ == State::Save) {
      if (networkSynced()) finish(true);
      else if (now - stateSince_ > 30000) {
        Serial.println("[AN] Saved on root; waiting for missing acknowledgements or node reconnection");
        stateSince_ = now;
      }
    }
  }

 private:
  enum class State { Idle, Identify, Wait, Settle, Capture, Review, Save };
  // One NVS key makes A/n/version a single persisted record.
  struct Blob { uint32_t magic = 0, revision = 0; double a = 0, exponent = 0; };
  struct Peer {
    unsigned board = 0;
    uint32_t id = 0, revision = 0, lock = 0, seen = 0, sync = 0;
    bool identified = false;
  };
  std::vector<Peer> peers_;
  State state_ = State::Idle;
  uint8_t selected_ = 0, point_ = 0, attempt_ = 0;
  uint32_t session_ = 0, remoteSession_ = 0, revision_ = 0, round_ = 0;
  uint32_t lastSend_ = 0, identifySince_ = 0, stateSince_ = 0, remoteSeen_ = 0;
  uint32_t stepSinceMs_ = 0, roundSinceMs_ = 0;
  bool persisted_ = false, acceptable_ = false, abort_ = false;
  bool remeasuring_ = false;
  double baselineA_ = -60, baselineN_ = 2;
  bool diagnostic_ = false;
  uint8_t diagnosticDistance_ = 1, diagnosticRound_ = 0;
  std::array<double, 5> diagnosticForward_{}, diagnosticReverse_{};
  std::array<bool, 5> diagnosticForwardValid_{}, diagnosticReverseValid_{};
  bool syncPending_ = false;
  bool identificationAnnounced_ = false;
  uint8_t syncAttempts_ = 0;
  uint32_t syncEpoch_ = 0, remoteSyncEpoch_ = 0;
  std::array<double, 5> forward_{}, reverse_{};
  Calibration::Fit candidate_, forwardFit_, reverseFit_;
  static uint32_t newId() { uint32_t id; do { id = esp_random(); } while (!id); return id; }
  static bool validParameters(double a, double exponent) {
    return std::isfinite(a) && a >= -100 && a <= -10 &&
        std::isfinite(exponent) && exponent >= 0.5 && exponent <= 6;
  }
  static bool validBlob(const Blob &blob) {
    return blob.magic == 0x414E3031 && blob.revision && validParameters(blob.a, blob.exponent);
  }
  static bool store(double a, double exponent, uint32_t revision) {
    Blob blob{}; blob.magic = 0x414E3031; blob.revision = revision; blob.a = a; blob.exponent = exponent;
    Preferences prefs;
    if (!prefs.begin("rssi-model", false)) return false;
    const bool ok = prefs.putBytes("model", &blob, sizeof(blob)) == sizeof(blob);
    prefs.end();
    return ok;
  }
  const Peer *findPeer(unsigned board) const {
    for (const auto &peer : peers_) if (peer.board == board) return &peer;
    return nullptr;
  }
  bool fresh(const Peer &peer) const {
    return mesh.isConnected(peer.id);
  }
  bool connectedPeersSynced() const {
    for (uint32_t id : mesh.getNodeList()) {
      bool acknowledged = false;
      for (const auto &peer : peers_) {
        if (peer.id == id && peer.revision == revision_ && peer.sync == syncEpoch_ &&
            peer.lock == (localSession() ? session_ : uint32_t(0))) acknowledged = true;
      }
      if (!acknowledged) return false;
    }
    return true;
  }
  bool allLocked() const {
    for (unsigned board = 2; board <= macList.size(); ++board) {
      const Peer *peer = findPeer(board);
      if (!peer || !fresh(*peer) || peer->lock != session_ ||
          peer->revision != revision_ || peer->sync != syncEpoch_) return false;
    }
    return true;
  }
  void send(JsonDocument &doc) {
    String message; serializeJson(doc, message);
    if (timeSlicing.isRoot()) mesh.sendBroadcast(message);
    else mesh.sendSingle(timeSlicing.rootNodeId(), message);
  }
  void hello() {
    if (!meshStarted || !mesh.isConnected(timeSlicing.rootNodeId())) return;
    JsonDocument doc;
    doc["Mode"] = "an"; doc["Action"] = "hello"; doc["Board"] = BOARD_NUMBER;
    doc["Revision"] = revision_; doc["Session"] = remoteSession_;
    doc["SyncEpoch"] = remoteSyncEpoch_;
    doc["Identified"] = remoteSession_ && millis() - identifySince_ >= 7000;
    JsonArray ap = doc["AP"].to<JsonArray>(); for (uint8_t byte : AP_self_mac) ap.add(byte);
    send(doc);
  }
  void broadcastState() {
    JsonDocument doc;
    doc["Mode"] = "an"; doc["Action"] = "state";
    doc["Session"] = localSession() ? session_ : uint32_t(0);
    doc["Selected"] = selected_; doc["Revision"] = revision_;
    doc["SyncEpoch"] = syncEpoch_;
    doc["A"] = A; doc["n"] = n; doc["Persist"] = persisted_;
    send(doc);
  }
  double distance() const {
    return diagnostic_ ? double(diagnosticDistance_) : point_ < 5 ? double(point_ + 1) : 2.5;
  }
  void prompt() {
    state_ = State::Wait;
    if (diagnostic_) {
      Serial.printf("[DIAG] Place the pair %.1f m apart. Keep both boards fixed for ALL five windows. Enter next | cancel\n", distance());
      return;
    }
    Serial.printf("[AN] %sPlace selected boards %.1f metres apart, same plane. Enter next | cancel\n",
                  point_ == 5 ? "Validation: " : "", distance());
  }
  void finish(bool saved) {
    const bool diagnosticComplete = diagnostic_ && diagnosticRound_ == 5 && !abort_;
    state_ = State::Idle; session_ = 0; abort_ = false;
    syncPending_ = true; syncAttempts_ = 0;
    if (meshStarted) broadcastState();
    if (diagnostic_) {
      Serial.println(diagnosticComplete ? "[DIAG] Complete. A/n and saved calibration unchanged"
                                       : "[DIAG] Cancelled. A/n and saved calibration unchanged");
      diagnostic_ = false;
    } else Serial.println(saved ? "[AN] Calibration saved; all configured nodes acknowledged active A/n"
                                : "[AN] Calibration cancelled; previous model retained");
  }
  bool reading(JsonVariantConst report, const char *key, double &value) {
    if (report["RoundId"].as<uint32_t>() != round_ ||
        strcmp(report["Status"] | "", "ok") != 0 ||
        !report["Samples"][key].is<unsigned>() ||
        report["SampleWindowMs"].as<unsigned>() != TimeSliceProtocol::CALIBRATION_SAMPLE_US / 1000 ||
        !report["FilteredRSSI"][key].is<double>() ||
        !report["RSSISpread"][key].is<double>() ||
        !report["ObservedWindowMs"][key].is<unsigned>() ||
        !report["PacketSpanMs"][key].is<unsigned>()) return false;
    value = report["FilteredRSSI"][key];
    const double spread = report["RSSISpread"][key];
    Serial.printf("[AN] %s receiver: RSSI=%.2f dBm, samples=%u/300 minimum, spread=%.2f dB\n",
                  strcmp(key, "2") == 0 ? "root" : "selected node", value,
                  report["Samples"][key].as<unsigned>(), spread);
    Serial.printf("[AN] Receiver observed window=%.3f s (scheduled 10.000 s), first-to-last packet span=%.3f s\n",
                  double(report["ObservedWindowMs"][key].as<unsigned>()) / 1000,
                  double(report["PacketSpanMs"][key].as<unsigned>()) / 1000);
    return report["Samples"][key].as<unsigned>() >= TimeSliceProtocol::CALIBRATION_MIN_SAMPLES &&
        std::isfinite(value) && value >= -127 && value <= 0 &&
        std::isfinite(spread) && spread >= 0 && spread <= 4;
  }
  void readCapture() {
    Serial.printf("[AN] Round exchange elapsed=%.3f s; elapsed since next=%.3f s (includes retries/reconnection)\n",
                  double(uint32_t(millis() - roundSinceMs_)) / 1000,
                  double(uint32_t(millis() - stepSinceMs_)) / 1000);
    double forward = NAN, reverse = NAN;
    const bool gotForward = reading(roundReports["1"], "2", forward);
    const bool gotReverse = reading(roundReports["2"], "1", reverse);
    if (diagnostic_) {
      diagnosticCapture(forward, reverse, gotForward, gotReverse);
      return;
    }
    if (!gotForward || !gotReverse) {
      if (attempt_ < 3) {
        Serial.println("[AN] Insufficient/unstable readings; automatically collecting another window");
        state_ = State::Settle; stateSince_ = millis();
      } else {
        Serial.println("[AN] Three captures failed. Keep path clear; enter next to retry, or cancel");
        state_ = State::Wait;
      }
      return;
    }
    if (point_ < 5) {
      forward_[point_] = forward; reverse_[point_] = reverse;
      if (remeasuring_) { point_ = 5; remeasuring_ = false; }
      else if (++point_ < 5) { prompt(); return; }
      forwardFit_ = Calibration::fit(forward_); reverseFit_ = Calibration::fit(reverse_);
      std::array<double, 5> shared{};
      for (unsigned i = 0; i < 5; ++i) shared[i] = (forward_[i] + reverse_[i]) / 2;
      candidate_ = Calibration::fit(shared);
      printFit("Root receiving", forwardFit_); printFit("Node receiving", reverseFit_);
      printFit("Shared baseline", candidate_);
      acceptable_ = forwardFit_.valid() && reverseFit_.valid() && candidate_.valid();
      const auto check = Calibration::checkTraining(candidate_, forward_, reverse_);
      Serial.println("[AN] Distance checks (candidate estimates; old model remains active):");
      for (unsigned i = 0; i < 5; ++i) {
        Serial.printf("[AN] Actual %u m -> %.2f / %.2f m%s\n", i + 1,
                      check.forward[i], check.reverse[i], check.recheck[i] ? " RECHECK (>30% error)" : "");
      }
      if (!check.passed) {
        acceptable_ = false;
        Serial.println("[AN] Model cannot explain all distances within 30%; remeasure flagged distances. No readings were discarded");
      }
      if (std::fabs(forwardFit_.a - reverseFit_.a) > 6 ||
          std::fabs(forwardFit_.n - reverseFit_.n) > 1) {
        Serial.println("[AN] Large directional disagreement; shared model rejected"); acceptable_ = false;
      }
      if (!acceptable_) {
        Serial.println("[AN] Fit rejected: requires A -100..-10, n 0.5..6, RMSE <=4 dB and all distance checks. Use remeasure <1..5> | retry | cancel");
        state_ = State::Review;
      } else prompt();
    } else {
      const auto check = Calibration::checkValidation(candidate_, baselineA_, baselineN_, forward, reverse);
      Serial.printf("[AN] Validation actual=2.50 m; fitted=%.2f / %.2f m; previous=%.2f / %.2f m\n",
                    check.forward, check.reverse, check.previousForward, check.previousReverse);
      Serial.printf("[AN] Validation mean absolute error: fitted=%.3f m, previous=%.3f m\n",
                    check.mae, check.previousMae);
      acceptable_ = check.passed();
      state_ = State::Review;
      if (!check.withinTolerance) Serial.println("[AN] Validation failed: exceeds 30% distance error");
      if (!check.noRegression) Serial.println("[AN] Validation failed: candidate error exceeds previous model (2 cm allowance)");
      Serial.println(acceptable_
          ? "[AN] Validation accepted at this point; no accuracy claim for other locations. Use save | remeasure <1..5> | retry | cancel"
          : "[AN] Previous globals/NVS retained. Use remeasure <1..5> | retry | cancel");
    }
  }
  static void printFit(const char *label, const Calibration::Fit &fit) {
    Serial.printf("[AN] %s: A=%.4f dBm, n=%.4f, RMSE=%.3f dB\n", label, fit.a, fit.n, fit.rmse);
  }
  void diagnosticCapture(double forward, double reverse, bool forwardValid, bool reverseValid) {
    const unsigned i = diagnosticRound_;
    diagnosticForward_[i] = forward; diagnosticReverse_[i] = reverse;
    diagnosticForwardValid_[i] = forwardValid; diagnosticReverseValid_[i] = reverseValid;
    Serial.printf("[DIAG] Window %u/5: root=%.2f dBm [%s], node=%.2f dBm [%s]\n", i + 1,
        forward, forwardValid ? "valid" : "flagged/missing", reverse, reverseValid ? "valid" : "flagged/missing");
    printDiagnosticRadio(roundReports["1"], "2", "Root 01");
    printDiagnosticRadio(roundReports["2"], "1", "Selected node");
    if (++diagnosticRound_ < 5) {
      attempt_ = 0; state_ = State::Settle; stateSince_ = millis();
      Serial.println("[DIAG] Next window will start automatically after mesh readiness. DO NOT move either board");
      return;
    }
    Serial.printf("[DIAG] Fixed-position summary at user-labelled %.1f m (not fitted distances):\n", distance());
    for (unsigned row = 0; row < 5; ++row)
      Serial.printf("[DIAG] %u: root=%.2f [%s], node=%.2f [%s] dBm\n", row + 1,
          diagnosticForward_[row], diagnosticForwardValid_[row] ? "valid" : "flagged/missing",
          diagnosticReverse_[row], diagnosticReverseValid_[row] ? "valid" : "flagged/missing");
    printWindowSummary("Root receiver", Calibration::summarizeWindows(diagnosticForward_, diagnosticForwardValid_));
    printWindowSummary("Node receiver", Calibration::summarizeWindows(diagnosticReverse_, diagnosticReverseValid_));
    Serial.println("[DIAG] Compare config lines across windows/boards. TX cap is configured maximum, not measured RF output");
    Serial.println("[DIAG] Repeat the same placement, or move to 2 m and run an diag <board> 2");
    finish(false);
  }
  static void printDiagnosticRadio(JsonVariantConst report, const char *key, const char *label) {
    Serial.printf("[DIAG] %s radio configuration: ", label);
    if (report["RadioConfig"].is<JsonObjectConst>()) {
      serializeJson(report["RadioConfig"], Serial); Serial.println();
      if (!(report["RadioConfig"]["ReadOK"] | false))
        Serial.println("[DIAG] WARNING: one or more radio configuration reads failed");
    } else Serial.println("unavailable (missing report or node firmware needs updating)");
    if (report["ReceivedChannel"][key].is<unsigned>()) {
      Serial.printf("[DIAG] Observed RX channel=%u, changed during capture=%s\n",
          report["ReceivedChannel"][key].as<unsigned>(), (report["ChannelChanged"][key] | false) ? "YES" : "no");
    }
  }
  static void printWindowSummary(const char *label, const Calibration::WindowSummary &summary) {
    if (!summary.count) { Serial.printf("[DIAG] %s: no valid windows; no stability conclusion\n", label); return; }
    Serial.printf("[DIAG] %s: valid=%u/5, mean=%.2f, min=%.2f, max=%.2f, between-window SD=%.2f, range=%.2f dB\n",
        label, summary.count, summary.mean, summary.minimum, summary.maximum, summary.spread, summary.maximum - summary.minimum);
    if (summary.count < 3) Serial.println("[DIAG] Too few valid windows for a useful repeatability check");
    else if (summary.maximum - summary.minimum > 3)
      Serial.println("[DIAG] RSSI changed by >3 dB between valid windows at this fixed placement; investigate repeatability");
  }
};
