#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <array>
#include <vector>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_timer.h>
#include "painlessMesh.h"
#include "time_slicing_protocol.h"
#include "../shared/rssi_distance.h"
#include "../shared/round_safety.h"
#include "../shared/calibration_math.h"

// painlessMesh 1.5.7 allocates an ESP32 mutex on every init(), but its
// stop() does not release it. Release it only after stopping, outside callbacks,
// so repeated measurement rounds do not leak one mutex per mesh restart.
class MeasurementMesh : public painlessMesh {
 public:
  bool sendBroadcast(String message, bool includeSelf = false) {
    const bool queued = painlessMesh::sendBroadcast(message, includeSelf);
    if (queued) {
      broadcastSeen_ = true;
      lastBroadcastMs_ = millis();
    }
    return queued;
  }

  // Stretch brief application sends into a visible burst; this is a queue
  // activity indicator, not a radio delivery acknowledgement.
  bool broadcastIndicatorActive() const {
    return broadcastSeen_ && uint32_t(millis() - lastBroadcastMs_) < 600;
  }

  void stopForMeasurement() {
    painlessMesh::stop();
    if (xSemaphore != nullptr) {
      vSemaphoreDelete(xSemaphore);
      xSemaphore = nullptr;
    }
  }

 private:
  bool broadcastSeen_ = false;
  uint32_t lastBroadcastMs_ = 0;
};

class TimeSlicingController {
 public:
  using Mac = std::array<uint8_t, 6>;
  using Phase = RoundSafety::Phase;

  JsonDocument slotRSSI;
  std::vector<uint32_t> slotPacketCounts;
  JsonDocument reports;
  JsonDocument distances;        // Other station MAC -> estimated metres.
  JsonDocument distanceReports;  // Reporter station MAC -> distances.
  uint32_t distanceRoundId = 0;
  JsonDocument sniffReports;  // Reporter MAC -> full target capture report.
  JsonDocument sniffCycleReports;  // Cycle -> reporter MAC -> full report.
  JsonDocument sniffAverageDistances;  // Reporter MAC -> target MAC -> mean metres.
  JsonDocument sniffAverageCounts;     // Reporter MAC -> number of valid cycles.
  JsonDocument channelDiscoveryReports;
  uint8_t discoveredChannel = 0;

  TimeSlicingController(MeasurementMesh &mesh, const uint8_t *selfAP,
                        const uint8_t *selfStation, const uint8_t *rootAP,
                        bool &meshStarted, String &mode, uint8_t &meshChannel,
                        void (*stopMesh)(), bool (*restartMesh)())
      : mesh_(mesh), selfAP_(selfAP), selfStation_(selfStation), rootAP_(rootAP),
        meshStarted_(meshStarted), mode_(mode), meshChannel_(meshChannel), stopMesh_(stopMesh),
        restartMesh_(restartMesh) {}

  bool isRoot() const { return memcmp(selfAP_, rootAP_, 6) == 0; }
  uint32_t rootNodeId() const {
    return painlessmesh::tcp::encodeNodeId(rootAP_);
  }
  bool busy() const { return phase_ != Phase::IDLE && phase_ != Phase::FAILED; }
  Phase phase() const { return phase_; }
  bool timingCritical() const { return RoundSafety::timingCritical(phase_); }
  bool roundCancelled() const { return cancelled_; }
  TimeSliceProtocol::RadioActivity radioActivity() const {
    using TimeSliceProtocol::RadioActivity;
    if (phase_ != Phase::MEASURING) return RadioActivity::None;
    if (sniffRound_) {
      if (discoveryRound_) return RadioActivity::None;
      const int64_t elapsed = esp_timer_get_time() - localStartUs_;
      return elapsed >= 0 && elapsed < TimeSliceProtocol::sniffDurationUs(false)
          ? RadioActivity::Receive : RadioActivity::Waiting;
    }
    return TimeSliceProtocol::measurementActivity(esp_timer_get_time(), localStartUs_,
        table_.size(), selfIndex_, calibrationRound_);
  }
  uint32_t roundId() const { return round_; }
  bool beginCalibrationRound(const std::vector<Mac> &pair, uint32_t session) {
    if (!session || pair.size() != 2 || !beginConfiguration(pair)) return false;
    calibrationRound_ = true;
    calibrationSession_ = session;
    return true;
  }
  bool networkReadyForConfiguration(size_t stationCount) {
    return meshStarted_ && !busy() && settled() && stationCount > 0 &&
           mesh_.getNodeList().size() >= stationCount - 1;
  }
  void meshRecovered() {
    if (phase_ != Phase::FAILED || !meshStarted_) return;
    if (cancelled_) {
      phase_ = Phase::IDLE;
      mode_ = "mesh";
      sendCancelAck();
      return;
    }
    mode_ = "mesh";
    phaseSinceMs_ = millis();
    lastMessageMs_ = millis() - RETRY_MS;
    if (isRoot()) {
      phase_ = Phase::COLLECTING;
      JsonDocument report;
      makeReport(report);
      reports[String(static_cast<unsigned long>(selfIndex_ + 1))].set(report);
      storeDistanceReport(report, selfIndex_);
      reportReceived_[selfIndex_] = true;
    } else {
      phase_ = Phase::REPORTING;
    }
    Serial.printf("[ROUND %u] Mesh recovered; resuming result exchange\n", round_);
  }

  void topologyChanged() {
    lastUnstableMs_ = millis();
  }
  void timeAdjusted(int32_t offset) {
    // Small corrections are tolerated by the 500 ms guards. Re-evaluate the
    // mesh-to-local deadline each loop until the radio preparation boundary.
    if (offset > 50000 || offset < -50000) {
      lastUnstableMs_ = millis();
      if (phase_ == Phase::ARMED) scheduleInvalid_ = true;
    }
  }

  bool beginConfiguration(const std::vector<Mac> &table) {
    if (!isRoot() || !meshStarted_ || busy() || !validTable(table) ||
        !std::isfinite(A) || !std::isfinite(n) || n <= 0) return false;
    uint32_t round;
    do { round = esp_random(); } while (round == 0 || round == round_);
    resetRound(table, round);
    ready_[selfIndex_] = true;
    peerIds_[selfIndex_] = mesh_.getNodeId();
    phase_ = Phase::WAIT_READY;
    phaseSinceMs_ = millis();
    lastMessageMs_ = millis() - RETRY_MS;
    mode_ = "conf";
    Serial.printf("[ROUND %u] Waiting for %u ready nodes\n", round_,
                  static_cast<unsigned>(table_.size()));
    return true;
  }

  bool beginSniff(const std::vector<Mac> &table, const Mac &target,
                  uint8_t channel = 0, uint8_t cycle = 0, bool continuation = false) {
    if (channel && cycle == 0) cycle = 1;
    if (cycle > TimeSliceProtocol::SNIFF_CYCLES || (!channel && cycle != 0)) return false;
    if (channel > 13 || !beginConfiguration(table)) return false;
    sniffRound_ = true;
    discoveryRound_ = channel == 0;
    target_ = target;
    targetChannel_ = channel;
    captureCycle_ = cycle;
    mode_ = discoveryRound_ ? "scan" : "sniff";
    if (!continuation) {
      channelDiscoveryReports.clear();
      sniffCycleReports.clear();
      sniffAverageDistances.clear();
      sniffAverageCounts.clear();
      discoveredChannel = channel;
    }
    sniffReports.clear();
    if (!discoveryRound_) Serial.printf("[SNIFF] Capture cycle %u/%u, %u seconds\n",
        captureCycle_, TimeSliceProtocol::SNIFF_CYCLES, unsigned(TimeSliceProtocol::SNIFF_US / 1000000));
    Serial.printf("[SNIFF] %s for %s%s\n",
                  discoveryRound_ ? "Channel discovery" : "Fixed-channel capture",
                  macString(target_).c_str(), discoveryRound_ ? "" : " scheduled");
    return true;
  }

  // Called by handleConfigMode() after validating a temporary ordered table.
  // No radio changes happen in a mesh callback.
  bool acceptConfiguration(JsonDocument &doc, const std::vector<Mac> &table,
                            uint32_t from) {
    bool sniff = false, discovery = false;
    Mac target{};
    uint8_t channel = 0, cycle = 0;
    if (!readJob(doc, sniff, discovery, target, channel, cycle)) return false;
    const bool calibration = doc["Calibration"] | false;
    if (calibration && (sniff || table.size() != 2 ||
        !doc["CalibrationSession"].is<uint32_t>() ||
        doc["CalibrationSession"].as<uint32_t>() == 0)) return false;
    if (isRoot() || from != rootNodeId() || !validTable(table) ||
        !std::isfinite(A) || !std::isfinite(n) || n <= 0 ||
        !validEnvelope(doc, 1) || !doc["RoundId"].is<uint32_t>() ||
        doc["RoundId"].as<uint32_t>() == 0 ||
        !parametersMatch(doc, hashTable(table))) return false;
    uint32_t round = doc["RoundId"];
    if (round == round_) {
      // Retransmission must not reset an armed/running round or reorder slots.
      if (table != table_ || !jobMatches(doc) || (phase_ != Phase::CONFIGURED &&
          phase_ != Phase::PREPARED && phase_ != Phase::ARMED)) return false;
      lastMessageMs_ = millis() - RETRY_MS;
      return true;
    }
    if (busy()) return false;
    resetRound(table, round);
    calibrationRound_ = calibration;
    calibrationSession_ = calibration ? doc["CalibrationSession"].as<uint32_t>() : 0;
    sniffRound_ = sniff;
    discoveryRound_ = discovery;
    target_ = target;
    targetChannel_ = channel;
    captureCycle_ = cycle;
    phase_ = Phase::CONFIGURED;
    phaseSinceMs_ = millis();
    lastMessageMs_ = millis() - RETRY_MS;
    return true;
  }

  void armConfiguredRound() {
    if (phase_ == Phase::CONFIGURED) {
      phase_ = Phase::PREPARED;
      mode_ = sniffRound_ ? (discoveryRound_ ? "scan" : "sniff") : "conf";
    }
  }

  bool handleControl(JsonDocument &doc, uint32_t from) {
    const char *action = doc["Action"] | "";
    if (!jobMatches(doc) || !doc["RoundId"].is<uint32_t>() ||
        doc["RoundId"].as<uint32_t>() != round_ || round_ == 0 ||
        !doc["ConfigHash"].is<uint32_t>() ||
        doc["ConfigHash"].as<uint32_t>() != hash_) return false;

    if (strcmp(action, "ready") == 0) {
      if (!isRoot() || phase_ != Phase::WAIT_READY || !validEnvelope(doc, 0))
        return false;
      size_t slot;
      if (!reporterSlot(doc, slot) || slot == selfIndex_ ||
          !macMatches(doc["StationMAC"], table_[slot])) return false;
      for (size_t i = 0; i < peerIds_.size(); ++i) {
        if (i != slot && peerIds_[i] == from) return false;
      }
      const bool firstReady = !ready_[slot];
      ready_[slot] = true;
      peerIds_[slot] = from;
      if (firstReady) {
        size_t count = 0;
        for (bool ready : ready_) if (ready) ++count;
        Serial.printf("[ROUND %u] Ready: %s, slot %u (%u/%u including root)\n",
                      round_, macString(table_[slot]).c_str(),
                      static_cast<unsigned>(slot + 1), static_cast<unsigned>(count),
                      static_cast<unsigned>(table_.size()));
      }
      return true;
    }

    if (strcmp(action, "start") == 0) {
      if (isRoot() || from != rootNodeId() || !validEnvelope(doc, 1) ||
          !parametersMatch(doc, hash_) || !doc["StartTimeUs"].is<uint32_t>())
        return false;
      uint32_t start = doc["StartTimeUs"];
      if (phase_ == Phase::ARMED) return start == meshStartUs_;
      if (phase_ != Phase::PREPARED ||
          TimeSliceProtocol::remainingUs(start, mesh_.getNodeTime()) <=
              static_cast<int32_t>(TimeSliceProtocol::PREPARE_US + 250000))
        return false;
      meshStartUs_ = start;
      phase_ = Phase::ARMED;
      lastMessageMs_ = millis();
      Serial.printf("[ROUND %u] Start scheduled at mesh time %u us\n",
                    round_, meshStartUs_);
      return true;
    }

    if (strcmp(action, "cancel") == 0) {
      if (isRoot() || from != rootNodeId() || !validEnvelope(doc, 1)) return false;
      cancelled_ = true;
      if (phase_ == Phase::MEASURING) {
        cancelRadioPending_ = true;  // Restore the radio later, outside callbacks.
        return true;
      }
      phase_ = Phase::IDLE;
      mode_ = "mesh";
      sendCancelAck();
      return true;
    }

    if (strcmp(action, "cancel_ack") == 0) {
      if (!isRoot() || !cancelled_ || !validEnvelope(doc, 0)) return false;
      size_t slot;
      if (!reporterSlot(doc, slot) || slot == selfIndex_ ||
          !macMatches(doc["StationMAC"], table_[slot]) ||
          (peerIds_[slot] && peerIds_[slot] != from)) return false;
      peerIds_[slot] = from;
      if (!cancellation_.acknowledged(slot)) Serial.printf("[ROUND %u] Cancel acknowledged: %s\n",
                                                   round_, macString(table_[slot]).c_str());
      cancellation_.acknowledge(slot);
      return true;
    }

    if (strcmp(action, "rssi_report") == 0 || strcmp(action, "sniff_report") == 0) {
      if (isRoot() && cancelled_ && validEnvelope(doc, 0)) {
        size_t slot;
        if (!reporterSlot(doc, slot) || slot == selfIndex_ || peerIds_[slot] != from)
          return false;
        Serial.printf("[ROUND %u] Ignored late report from %s: round cancelled\n",
                      round_, macString(table_[slot]).c_str());
        sendCancel(from);
        return true;
      }
      return acceptReport(doc, from);
    }
    if (strcmp(action, "report_ack") == 0) {
      if (isRoot() || from != rootNodeId() || !validEnvelope(doc, 1) ||
          phase_ != Phase::REPORTING) return false;
      phase_ = Phase::IDLE;
      Serial.printf("[ROUND %u] Root acknowledged results\n", round_);
      return true;
    }
    return false;
  }

  void update() {
    using namespace TimeSliceProtocol;
    const int64_t serviceUs = esp_timer_get_time();
    if (timingCritical() && lastUpdateUs_ && serviceUs > lastUpdateUs_) {
      const uint64_t gap = uint64_t(serviceUs - lastUpdateUs_);
      if (gap > maxServiceGapUs_) maxServiceGapUs_ = gap;
    }
    lastUpdateUs_ = serviceUs;
    uint32_t nowMs = millis();
    if (cancelRadioPending_) {
      cancelRadioPending_ = false;
      endRadio();
      mode_ = "mesh";
      if (!restartMesh_()) { phase_ = Phase::FAILED; return; }
      phase_ = Phase::IDLE;
      sendCancelAck();
      return;
    }
    if (phase_ == Phase::CANCELLING) {
      if (cancellation_.status(nowMs) != RoundSafety::Cancellation::Status::Pending) {
        for (size_t i = 0; i < table_.size(); ++i) {
          if (!cancellation_.acknowledged(i)) Serial.printf("[ROUND %u] Cancel ACK timeout: %s\n",
                                                   round_, macString(table_[i]).c_str());
        }
        phase_ = Phase::IDLE;
        Serial.println("[ROUND] Cancelled; no result table or average produced. Commands enabled");
      } else if (meshStarted_ && nowMs - lastMessageMs_ >= RETRY_MS) {
        sendCancel();
      }
      return;
    }
    if (phase_ == Phase::CHANNEL_WAIT) {
      if (nowMs - phaseSinceMs_ > READY_TIMEOUT_MS) {
        phase_ = Phase::IDLE;
        Serial.println("[ERROR] Mesh did not settle before the next sniff cycle; operation stopped");
      } else if (nowMs - phaseSinceMs_ >= 3000 && settled() && meshStarted_ &&
                 mesh_.getNodeList().size() >= table_.size() - 1) {
        const auto table = table_;
        const auto target = target_;
        phase_ = Phase::IDLE;
        if (!beginSniff(table, target, pendingChannel_, pendingCycle_, true))
          Serial.println("[ERROR] Could not start fixed-channel sniff");
      }
      return;
    }
    if (phase_ == Phase::WAIT_READY) {
      if (nowMs - phaseSinceMs_ > READY_TIMEOUT_MS) {
        for (size_t i = 0; i < table_.size(); ++i) {
          if (!ready_[i]) Serial.printf("[ROUND %u] Missing ready: %s\n",
                                       round_, macString(table_[i]).c_str());
        }
        beginCancellation("missing ready replies");
      } else if (allSet(ready_) && settled()) {
        meshStartUs_ = mesh_.getNodeTime() + START_LEAD_US;
        phase_ = Phase::ARMED;
        lastMessageMs_ = nowMs - RETRY_MS;
        Serial.printf("[ROUND %u] All stations ready; start in %u ms (mesh time %u us)\n",
                      round_, START_LEAD_US / 1000, meshStartUs_);
        sendControl("start", true);
      } else if (nowMs - lastMessageMs_ >= RETRY_MS) {
        sendConfiguration();
      }
      return;
    }
    if (phase_ == Phase::PREPARED) {
      if (nowMs - phaseSinceMs_ > READY_TIMEOUT_MS + 20000) {
        phase_ = Phase::IDLE;
        mode_ = "mesh";
        Serial.println("[ROUND] No start received; remaining in mesh");
      } else if (settled() && nowMs - lastMessageMs_ >= RETRY_MS) {
        sendReady();
      }
      return;
    }
    if (phase_ == Phase::ARMED) {
      int32_t remaining = remainingUs(meshStartUs_, mesh_.getNodeTime());
      if (scheduleInvalid_) {
        finishRound(false, "clock changed after scheduling", false);
      } else if (remaining <= static_cast<int32_t>(PREPARE_US)) {
        // Do not shift late slots: either meet the shared schedule or fail.
        if (remaining < static_cast<int32_t>(PREPARE_US - 250000)) {
          Serial.printf("[TIMING] Preparation remaining=%ld us; controller max gap=%llu us\n",
                        static_cast<long>(remaining), static_cast<unsigned long long>(maxServiceGapUs_));
          finishRound(false, "missed radio preparation deadline", false);
          return;
        }
        localStartUs_ = esp_timer_get_time() + remaining;
        stopMesh_();
        bool prepared = beginRadio();
        if (!prepared || esp_timer_get_time() >= localStartUs_) {
          finishRound(false, "radio preparation failed or finished late", true);
        } else {
          phase_ = Phase::MEASURING;
          mode_ = discoveryRound_ ? "scan" : "sniff";
          portENTER_CRITICAL(&mux_);
          capturing_ = true;
          portEXIT_CRITICAL(&mux_);
          if (sniffRound_) Serial.printf("[ROUND %u] Radio ready; %s for %u ms, channel=%u\n",
              round_, discoveryRound_ ? "channel discovery" : "target capture",
              unsigned(sniffDurationUs(discoveryRound_) / 1000), targetChannel_);
          else Serial.printf("[ROUND %u] Radio ready; %u ordered slots\n", round_,
                             static_cast<unsigned>(table_.size()));
        }
      } else if (isRoot() && nowMs - lastMessageMs_ >= RETRY_MS) {
        sendControl("start", true);
      }
      return;
    }
    if (phase_ == Phase::MEASURING) {
      int64_t nowUs = esp_timer_get_time();
      if (calibrationRound_) {
        for (size_t i = 0; i < calibrationWindows_.size(); ++i) {
          auto &window = calibrationWindows_[i];
          const int64_t start = localStartUs_ + int64_t(i) * slotDurationUs(true) + GUARD_US;
          const bool wasOpen = window.openedUs >= 0, wasClosed = window.closedUs >= 0;
          window.observe(nowUs, start, start + CALIBRATION_SAMPLE_US);
          if (!wasOpen && window.openedUs >= 0)
            Serial.printf("[AN] %s window started: slot %u, local %.3f s, scheduled 10.000 s\n",
                i == selfIndex_ ? "TX" : "RX", unsigned(i + 1), double(nowUs) / 1000000);
          if (!wasClosed && window.closedUs >= 0)
            Serial.printf("[AN] %s window ended: slot %u, observed %.3f s\n",
                i == selfIndex_ ? "TX" : "RX", unsigned(i + 1), double(window.elapsedMs()) / 1000);
        }
      }
      if (sniffRound_) {
        const int64_t elapsed = nowUs - localStartUs_;
        if (elapsed >= sniffDurationUs(discoveryRound_)) finishRound(true, "complete", true);
        else if (discoveryRound_) {
          const uint8_t channel = sniffChannel(elapsed, scanFirst_, scanCount_);
          if (channel != captureChannel_) {
            if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
              finishRound(false, "channel hopping failed", true);
            } else captureChannel_ = channel;
          }
        }
        return;
      }
      if (nowUs >= localStartUs_ + int64_t(table_.size()) * slotDurationUs(calibrationRound_)) {
        finishRound(true, "complete", true);
        return;
      }
      size_t slot;
      if (activeSlot(nowUs, localStartUs_, table_.size(), slot, calibrationRound_) &&
          slot == selfIndex_ && nowUs >= nextSendUs_) {
        bool available;
        portENTER_CRITICAL(&mux_);
        available = !txBusy_;
        if (available) txBusy_ = true;
        portEXIT_CRITICAL(&mux_);
        if (available) {
          makePayload(txPayload_, round_, hash_, static_cast<uint16_t>(slot),
                      ++sequence_);
          esp_err_t error = esp_now_send(BROADCAST_MAC, txPayload_, PAYLOAD_SIZE);
          if (error != ESP_OK) {
            portENTER_CRITICAL(&mux_);
            txBusy_ = false;
            ++sendErrors_;
            portEXIT_CRITICAL(&mux_);
          }
          nextSendUs_ = nowUs + (calibrationRound_ ? 20000 : 100000);
        }
      }
      return;
    }
    if (phase_ == Phase::REPORTING) {
      if (nowMs - phaseSinceMs_ > REPORT_TIMEOUT_MS) {
        phase_ = Phase::IDLE;
        Serial.println("[ROUND] Report timeout; results remain available locally");
      } else if (meshStarted_ && mesh_.isConnected(rootNodeId()) &&
                 nowMs - lastMessageMs_ >= RETRY_MS) {
        JsonDocument report;
        makeReport(report);
        sendJson(report, rootNodeId());
      }
      return;
    }
    if (phase_ == Phase::COLLECTING) {
      if (allSet(reportReceived_)) {
        Serial.println("[ROUND] All result reports received");
        completeCollection();
      } else if (nowMs - phaseSinceMs_ > REPORT_TIMEOUT_MS) {
        Serial.println("[ROUND] Result collection timeout; missing reports retained as absent");
        completeCollection();
      }
    }
  }

 private:
  struct Sample {
    int64_t sum = 0;
    uint32_t count = 0, lastSequence = 0;
    Calibration::Histogram histogram;
    int64_t firstPacketUs = 0, lastPacketUs = 0;
    uint8_t receivedChannel = 0;
    bool channelChanged = false;
  };
  static constexpr uint32_t RETRY_MS = 1000;
  static constexpr uint32_t READY_TIMEOUT_MS = 30000;
  static constexpr uint32_t REPORT_TIMEOUT_MS = 120000;
  inline static constexpr uint8_t BROADCAST_MAC[6] = {255, 255, 255, 255, 255, 255};
  inline static TimeSlicingController *active_ = nullptr;

  MeasurementMesh &mesh_;
  const uint8_t *selfAP_, *selfStation_, *rootAP_;
  bool &meshStarted_;
  String &mode_;
  uint8_t &meshChannel_;
  void (*stopMesh_)();
  bool (*restartMesh_)();
  Phase phase_ = Phase::IDLE;
  std::vector<Mac> table_;
  std::vector<Sample> samples_;
  std::vector<bool> ready_, reportReceived_;
  std::vector<uint32_t> peerIds_;
  size_t selfIndex_ = 0;
  uint32_t round_ = 0, hash_ = 0, meshStartUs_ = 0;
  uint32_t phaseSinceMs_ = 0, lastMessageMs_ = 0, lastUnstableMs_ = 0;
  uint32_t sequence_ = 0, sendErrors_ = 0;
  double roundA_ = -60.0, roundN_ = 2.0;
  bool sniffRound_ = false, discoveryRound_ = false;
  bool calibrationRound_ = false;
  uint32_t calibrationSession_ = 0;
  std::array<TimeSliceProtocol::WindowTiming, 2> calibrationWindows_{};
  uint32_t radioElapsedMs_ = 0;
  JsonDocument radioConfig_;
  Mac target_{};
  uint8_t targetChannel_ = 0, captureChannel_ = 0, scanFirst_ = 1, scanCount_ = 11;
  uint8_t captureCycle_ = 0, pendingCycle_ = 1, pendingChannel_ = 0;
  Sample targetSample_;
  std::array<uint32_t, 14> channelSamples_{};
  int64_t localStartUs_ = 0, nextSendUs_ = 0;
  bool scheduleInvalid_ = false, radioInitialized_ = false, resultOK_ = false;
  bool cancelled_ = false, cancelRadioPending_ = false;
  RoundSafety::Cancellation cancellation_;
  uint32_t cancelTimeoutMs_ = 10000;
  int64_t lastUpdateUs_ = 0;
  uint64_t maxServiceGapUs_ = 0;
  const char *resultReason_ = "not started";
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  bool capturing_ = false, txBusy_ = false;
  uint8_t txPayload_[TimeSliceProtocol::PAYLOAD_SIZE] = {};

  bool settled() const { return millis() - lastUnstableMs_ >= 3000; }
  static bool allSet(const std::vector<bool> &flags) {
    for (bool flag : flags) if (!flag) return false;
    return !flags.empty();
  }
  static bool validEnvelope(JsonDocument &doc, int direction) {
    return doc["RootDirection"].is<int>() &&
           doc["RootDirection"].as<int>() == direction;
  }
  static bool readJob(JsonDocument &doc, bool &sniff, bool &discovery,
                      Mac &target, uint8_t &channel, uint8_t &cycle) {
    const char *job = doc["Job"] | "";
    if (strcmp(job, "conf") == 0) { sniff = discovery = false; return true; }
    if (strcmp(job, "sniff") != 0 || !doc["DiscoverChannel"].is<bool>() ||
        !doc["SniffChannel"].is<unsigned>() ||
        doc["SniffChannel"].as<unsigned>() > 13 ||
        !doc["SniffMs"].is<unsigned>() ||
        !doc["SniffCycle"].is<unsigned>() ||
        !doc["SniffCycles"].is<unsigned>() ||
        doc["SniffCycles"].as<unsigned>() != TimeSliceProtocol::SNIFF_CYCLES ||
        doc["SniffCycle"].as<unsigned>() > TimeSliceProtocol::SNIFF_CYCLES ||
        !doc["TargetMAC"].is<JsonArray>()) return false;
    JsonArray bytes = doc["TargetMAC"];
    if (bytes.size() != 6) return false;
    for (size_t i = 0; i < 6; ++i) {
      if (!bytes[i].is<unsigned>() || bytes[i].as<unsigned>() > 255) return false;
      target[i] = bytes[i].as<unsigned>();
    }
    sniff = true;
    discovery = doc["DiscoverChannel"].as<bool>();
    channel = doc["SniffChannel"].as<unsigned>();
    cycle = doc["SniffCycle"].as<unsigned>();
    return doc["SniffMs"].as<unsigned>() ==
               unsigned(TimeSliceProtocol::sniffDurationUs(discovery) / 1000) &&
           (discovery ? channel == 0 && cycle == 0 : channel >= 1 && cycle >= 1);
  }
  bool jobMatches(JsonDocument &doc) const {
    bool sniff = false, discovery = false;
    Mac target{};
    uint8_t channel = 0, cycle = 0;
    return readJob(doc, sniff, discovery, target, channel, cycle) &&
           (doc["Calibration"] | false) == calibrationRound_ &&
           (!calibrationRound_ || doc["CalibrationSession"].as<uint32_t>() == calibrationSession_) &&
           sniff == sniffRound_ && (!sniff || (discovery == discoveryRound_ &&
           channel == targetChannel_ && target == target_ && cycle == captureCycle_));
  }
  static bool macMatches(JsonVariantConst value, const Mac &mac) {
    if (!value.is<JsonArrayConst>()) return false;
    JsonArrayConst bytes = value.as<JsonArrayConst>();
    if (bytes.size() != 6) return false;
    for (size_t i = 0; i < 6; ++i) {
      if (!bytes[i].is<unsigned>() || bytes[i].as<unsigned>() != mac[i])
        return false;
    }
    return true;
  }
  bool validTable(const std::vector<Mac> &table) const {
    if (table.empty() || table.size() > UINT16_MAX) return false;
    bool containsSelf = false;
    for (size_t i = 0; i < table.size(); ++i) {
      bool nonzero = false;
      for (uint8_t byte : table[i]) nonzero |= byte != 0;
      if (!nonzero || (table[i][0] & 1)) return false;
      containsSelf |= memcmp(table[i].data(), selfStation_, 6) == 0;
      for (size_t j = 0; j < i; ++j) if (table[i] == table[j]) return false;
    }
    if (!containsSelf) Serial.println("[ROUND] MacList must include this station MAC");
    return containsSelf;
  }
  static uint32_t hashTable(const std::vector<Mac> &table) {
    uint32_t hash = 2166136261u;
    for (const Mac &mac : table) {
      for (uint8_t byte : mac) hash = (hash ^ byte) * 16777619u;
    }
    return hash;
  }
  bool parametersMatch(JsonDocument &doc, uint32_t hash) const {
    const bool calibration = doc["Calibration"] | false;
    return doc["ConfigHash"].is<uint32_t>() &&
           doc["ConfigHash"].as<uint32_t>() == hash &&
           doc["Channel"].is<unsigned>() &&
           doc["Channel"].as<unsigned>() == meshChannel_ &&
           doc["SlotMs"].is<unsigned>() &&
           doc["SlotMs"].as<unsigned>() == TimeSliceProtocol::slotDurationUs(calibration) / 1000 &&
           (!calibration || (doc["SampleMs"].is<unsigned>() &&
               doc["SampleMs"].as<unsigned>() == TimeSliceProtocol::CALIBRATION_SAMPLE_US / 1000)) &&
           doc["GuardMs"].is<unsigned>() &&
           doc["GuardMs"].as<unsigned>() == TimeSliceProtocol::GUARD_US / 1000;
  }
  bool reporterSlot(JsonDocument &doc, size_t &slot) const {
    if (!doc["ReporterSlot"].is<unsigned>()) return false;
    unsigned value = doc["ReporterSlot"];
    if (value == 0 || value > table_.size()) return false;
    slot = value - 1;
    return true;
  }
  void resetRound(const std::vector<Mac> &table, uint32_t round) {
    table_ = table;
    samples_.assign(table.size(), Sample{});
    slotPacketCounts.assign(table.size(), 0);
    ready_.assign(table.size(), false);
    reportReceived_.assign(table.size(), false);
    cancelled_ = cancelRadioPending_ = false;
    maxServiceGapUs_ = 0;
    lastUpdateUs_ = esp_timer_get_time();
    peerIds_.assign(table.size(), 0);
    for (size_t i = 0; i < table.size(); ++i) {
      if (memcmp(table[i].data(), selfStation_, 6) == 0) selfIndex_ = i;
    }
    round_ = round;
    hash_ = hashTable(table);
    slotRSSI.clear();
    reports.clear();
    distances.clear();
    distanceReports.clear();
    distanceRoundId = round;
    roundA_ = A;
    roundN_ = n;
    scheduleInvalid_ = false;
    sequence_ = sendErrors_ = 0;
    resultOK_ = false;
    resultReason_ = "not finished";
    nextSendUs_ = 0;
    sniffRound_ = discoveryRound_ = false;
    calibrationRound_ = false;
    calibrationSession_ = 0;
    calibrationWindows_.fill(TimeSliceProtocol::WindowTiming{});
    radioElapsedMs_ = 0;
    radioConfig_.clear();
    targetSample_ = Sample{};
    channelSamples_.fill(0);
    targetChannel_ = captureChannel_ = 0;
    captureCycle_ = 0;
  }
  void envelope(JsonDocument &doc, const char *mode, const char *action,
                int direction) const {
    doc["Mode"] = mode;
    doc["Action"] = action;
    doc["RoundId"] = round_;
    doc["ConfigHash"] = hash_;
    doc["RootDirection"] = direction;
    doc["Sender ID/MAC"] = mesh_.getNodeId();
    doc["Job"] = sniffRound_ ? "sniff" : "conf";
    doc["Calibration"] = calibrationRound_;
    if (calibrationRound_) doc["CalibrationSession"] = calibrationSession_;
    if (sniffRound_) {
      doc["DiscoverChannel"] = discoveryRound_;
      doc["SniffChannel"] = targetChannel_;
      doc["SniffMs"] = unsigned(TimeSliceProtocol::sniffDurationUs(discoveryRound_) / 1000);
      doc["SniffCycle"] = captureCycle_;
      doc["SniffCycles"] = TimeSliceProtocol::SNIFF_CYCLES;
      JsonArray target = doc["TargetMAC"].to<JsonArray>();
      for (uint8_t byte : target_) target.add(byte);
    }
  }
  void parameters(JsonDocument &doc) const {
    doc["Channel"] = meshChannel_;
    doc["SlotMs"] = TimeSliceProtocol::slotDurationUs(calibrationRound_) / 1000;
    doc["SampleMs"] = TimeSliceProtocol::sampleDurationUs(calibrationRound_) / 1000;
    doc["GuardMs"] = TimeSliceProtocol::GUARD_US / 1000;
  }
  bool sendJson(JsonDocument &doc, uint32_t destination = 0) {
    lastMessageMs_ = millis();
    if (doc.overflowed()) return false;
    String message;
    if (serializeJson(doc, message) != measureJson(doc)) return false;
    return destination ? mesh_.sendSingle(destination, message)
                       : mesh_.sendBroadcast(message);
  }
  void sendConfiguration() {
    JsonDocument doc;
    envelope(doc, "conf", "prepare", 1);
    parameters(doc);
    JsonObject list = doc["MacList"].to<JsonObject>();
    for (size_t i = 0; i < table_.size(); ++i) {
      JsonArray bytes = list[String(static_cast<unsigned long>(i + 1))].to<JsonArray>();
      for (uint8_t byte : table_[i]) bytes.add(byte);
    }
    sendJson(doc);
  }
  void sendReady() {
    Serial.printf("[ROUND %u] Sending ready from slot %u to root %u\n",
              round_,
              static_cast<unsigned>(selfIndex_ + 1),
              static_cast<unsigned>(rootNodeId()));
    JsonDocument doc;
    envelope(doc, "conf", "ready", 0);
    doc["ReporterSlot"] = selfIndex_ + 1;
    JsonArray bytes = doc["StationMAC"].to<JsonArray>();
    for (uint8_t byte : table_[selfIndex_]) bytes.add(byte);
    sendJson(doc, rootNodeId());
  }
  void sendControl(const char *action, bool broadcast) {
    JsonDocument doc;
    envelope(doc, "conf", action, 1);
    parameters(doc);
    if (strcmp(action, "start") == 0) doc["StartTimeUs"] = meshStartUs_;
    sendJson(doc, broadcast ? 0 : rootNodeId());
  }

  void sendCancel(uint32_t destination = 0) {
    JsonDocument doc;
    envelope(doc, "conf", "cancel", 1);
    sendJson(doc, destination);
  }

  void sendCancelAck() {
    if (isRoot() || !meshStarted_) return;
    JsonDocument doc;
    envelope(doc, "conf", "cancel_ack", 0);
    doc["ReporterSlot"] = selfIndex_ + 1;
    JsonArray bytes = doc["StationMAC"].to<JsonArray>();
    for (uint8_t byte : table_[selfIndex_]) bytes.add(byte);
    sendJson(doc, rootNodeId());
  }

  void beginCancellation(const char *reason) {
    const bool armed = phase_ == Phase::ARMED;
    const uint32_t durationMs = sniffRound_
        ? uint32_t(TimeSliceProtocol::sniffDurationUs(discoveryRound_) / 1000)
        : uint32_t(table_.size() * int64_t(TimeSliceProtocol::slotDurationUs(calibrationRound_)) / 1000);
    // A peer may have already stopped its mesh and cannot hear cancel until
    // capture ends. Keep the old round identity until that return/report window.
    cancelTimeoutMs_ = armed ? durationMs + TimeSliceProtocol::START_LEAD_US / 1000 + READY_TIMEOUT_MS : 10000;
    cancelled_ = true;
    resultOK_ = false;
    resultReason_ = reason;
    cancellation_.begin(table_.size(), selfIndex_, millis(), cancelTimeoutMs_);
    reports.clear();
    distanceReports.clear();
    phase_ = Phase::CANCELLING;
    mode_ = "mesh";
    phaseSinceMs_ = millis();
    lastMessageMs_ = millis() - RETRY_MS;
    Serial.printf("[ROUND %u] Cancelling: %s; waiting for node acknowledgements (up to %u ms)\n",
                  round_, reason, cancelTimeoutMs_);
    sendCancel();
  }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  static void sentCallback(const esp_now_send_info_t *, esp_now_send_status_t status) {
#else
  static void sentCallback(const uint8_t *, esp_now_send_status_t status) {
#endif
    if (!active_) return;
    portENTER_CRITICAL(&active_->mux_);
    active_->txBusy_ = false;
    if (status != ESP_NOW_SEND_SUCCESS) ++active_->sendErrors_;
    portEXIT_CRITICAL(&active_->mux_);
  }
  static void sniffCallback(void *buffer, wifi_promiscuous_pkt_type_t type) {
    if (!active_ || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
    auto *packet = static_cast<wifi_promiscuous_pkt_t *>(buffer);
    auto &self = *active_;
    if (self.sniffRound_) {
      if (!TimeSliceProtocol::targetFrame(packet->payload, packet->rx_ctrl.sig_len,
                                         self.target_.data())) return;
      const int64_t now = esp_timer_get_time();
      const uint8_t channel = packet->rx_ctrl.channel;
      portENTER_CRITICAL(&self.mux_);
      if (self.capturing_ && now >= self.localStartUs_ &&
          now < self.localStartUs_ + TimeSliceProtocol::sniffDurationUs(self.discoveryRound_) &&
          channel >= 1 && channel <= 13 &&
          (self.discoveryRound_ || channel == self.targetChannel_)) {
        self.targetSample_.sum += packet->rx_ctrl.rssi;
        ++self.targetSample_.count;
        ++self.channelSamples_[channel];
      }
      portEXIT_CRITICAL(&self.mux_);
      return;
    }
    if (type != WIFI_PKT_MGMT) return;
    const uint8_t *payload = TimeSliceProtocol::measurementPayload(
        packet->payload, packet->rx_ctrl.sig_len);
    if (!payload) return;
    portENTER_CRITICAL(&self.mux_);
    size_t slot;
    uint32_t sequence = TimeSliceProtocol::read32(payload + 14);
    const int64_t nowUs = esp_timer_get_time();
    if (self.capturing_ && TimeSliceProtocol::activeSlot(nowUs,
          self.localStartUs_, self.table_.size(), slot, self.calibrationRound_) &&
        slot != self.selfIndex_ && TimeSliceProtocol::matchesMeasurement(
          payload, packet->payload + 10, self.table_[slot].data(), self.round_,
          self.hash_, slot, self.samples_[slot].lastSequence)) {
      auto &sample = self.samples_[slot];
      sample.sum += packet->rx_ctrl.rssi;
      if (self.calibrationRound_) {
        sample.histogram.add(packet->rx_ctrl.rssi);
        if (!sample.count) sample.firstPacketUs = nowUs;
        sample.lastPacketUs = nowUs;
        if (!sample.receivedChannel) sample.receivedChannel = packet->rx_ctrl.channel;
        else if (sample.receivedChannel != packet->rx_ctrl.channel) sample.channelChanged = true;
      }
      ++sample.count;
      sample.lastSequence = sequence;
    }
    portEXIT_CRITICAL(&self.mux_);
  }
  bool beginRadio() {
    active_ = this;
    txBusy_ = capturing_ = false;
    if (!WiFi.mode(WIFI_STA)) return false;
    uint8_t actual[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, actual) != ESP_OK ||
        memcmp(actual, selfStation_, 6) != 0 ||
        esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK)
      return false;
    if (sniffRound_) {
      wifi_country_t country{};
      if (esp_wifi_get_country(&country) != ESP_OK || country.schan < 1 ||
          country.nchan < 1 || country.schan + country.nchan - 1 > 13) return false;
      scanFirst_ = country.schan;
      scanCount_ = country.nchan;
      captureChannel_ = discoveryRound_ ? scanFirst_ : targetChannel_;
      if (captureChannel_ < scanFirst_ || captureChannel_ >= scanFirst_ + scanCount_ ||
          esp_wifi_set_channel(captureChannel_, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.println("[ERROR] Target channel is not supported by this board's country settings");
        return false;
      }
      wifi_promiscuous_filter_t filter{};
      filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
      if (discoveryRound_) Serial.printf("[SNIFF] Hopping channels %u..%u\n",
          scanFirst_, scanFirst_ + scanCount_ - 1);
      else Serial.printf("[SNIFF] Listening on channel %u\n", targetChannel_);
      return esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
             esp_wifi_set_promiscuous_rx_cb(sniffCallback) == ESP_OK &&
             esp_wifi_set_promiscuous(true) == ESP_OK;
    }
    if (esp_wifi_set_channel(meshChannel_, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
        esp_wifi_set_max_tx_power(60) != ESP_OK || esp_now_init() != ESP_OK) return false;
    radioInitialized_ = true;
    if (calibrationRound_) snapshotRadioConfiguration();
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = meshChannel_;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    return esp_now_add_peer(&peer) == ESP_OK &&
           esp_now_register_send_cb(sentCallback) == ESP_OK &&
           esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
           esp_wifi_set_promiscuous_rx_cb(sniffCallback) == ESP_OK &&
           esp_wifi_set_promiscuous(true) == ESP_OK;
  }
  void endRadio() {
    portENTER_CRITICAL(&mux_);
    capturing_ = false;
    portEXIT_CRITICAL(&mux_);
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    if (radioInitialized_) {
      esp_now_unregister_send_cb();
      esp_now_deinit();
      radioInitialized_ = false;
    }
    // Keep the station driver/netifs initialized. restartMesh() enables AP
    // mode again; tearing Wi-Fi down here can race pending stop events.
  }
  void snapshotRadioConfiguration() {
    radioConfig_.clear();
    uint8_t station[6] = {};
    uint8_t channel = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    int8_t txCap = 0;
    const bool macOK = esp_wifi_get_mac(WIFI_IF_STA, station) == ESP_OK;
    const bool channelOK = esp_wifi_get_channel(&channel, &secondary) == ESP_OK;
    const bool powerOK = esp_wifi_get_max_tx_power(&txCap) == ESP_OK;
    radioConfig_["ReadOK"] = macOK && channelOK && powerOK;
    if (macOK) {
      Mac stationMac{};
      memcpy(stationMac.data(), station, 6);
      radioConfig_["StationMAC"] = macString(stationMac);
    }
    Mac meshAP{};
    memcpy(meshAP.data(), selfAP_, 6);
    radioConfig_["MeshAPMAC"] = macString(meshAP);
    if (channelOK) radioConfig_["WiFiChannel"] = channel;
    if (powerOK) radioConfig_["TxPowerCapQuarterDbm"] = txCap;
    radioConfig_["RequestedTxPowerCapQuarterDbm"] = 60;
    radioConfig_["TxIntervalMs"] = 20;
    Serial.print("[RADIO] Calibration capture config: ");
    serializeJson(radioConfig_, Serial);
    Serial.println();
  }
  void makeReport(JsonDocument &doc) {
    envelope(doc, "mesh", sniffRound_ ? "sniff_report" : "rssi_report", 0);
    doc["ReporterSlot"] = selfIndex_ + 1;
    doc["Status"] = resultOK_ ? "ok" : "failed";
    doc["Reason"] = resultReason_;
    doc["SendErrors"] = sendErrors_;
    doc["ReporterMAC"] = macString(table_[selfIndex_]);
    doc["A"] = roundA_;
    doc["n"] = roundN_;
    if (calibrationRound_) {
      doc["SampleWindowMs"] = TimeSliceProtocol::CALIBRATION_SAMPLE_US / 1000;
      doc["RadioElapsedMs"] = radioElapsedMs_;
      doc["RadioConfig"].set(radioConfig_.as<JsonObjectConst>());
    }
    doc["Distances"].set(distances.as<JsonObjectConst>());
    doc["RSSI"].set(slotRSSI.as<JsonObjectConst>());
    JsonObject counts = doc["Samples"].to<JsonObject>();
    if (sniffRound_) {
      counts[macString(target_)] = targetSample_.count;
      if (discoveryRound_) {
        JsonObject channels = doc["ChannelSamples"].to<JsonObject>();
        for (unsigned channel = 1; channel <= 13; ++channel)
          channels[String(channel)] = channelSamples_[channel];
      }
      return;
    }
    for (size_t i = 0; i < slotPacketCounts.size(); ++i) {
      counts[String(static_cast<unsigned long>(i + 1))] = slotPacketCounts[i];
      if (calibrationRound_ && i != selfIndex_ && samples_[i].count) {
        const String key = String(static_cast<unsigned long>(i + 1));
        doc["FilteredRSSI"][key] = samples_[i].histogram.trimmedMean();
        doc["RSSISpread"][key] = samples_[i].histogram.spread();
        doc["ObservedWindowMs"][key] = calibrationWindows_[i].elapsedMs();
        doc["PacketSpanMs"][key] = uint32_t((samples_[i].lastPacketUs - samples_[i].firstPacketUs) / 1000);
        doc["ReceivedChannel"][key] = samples_[i].receivedChannel;
        doc["ChannelChanged"][key] = samples_[i].channelChanged;
      }
    }
  }
  void finishRound(bool ok, const char *reason, bool restoreRadio) {
    if (isRoot() && !ok && !restoreRadio) {
      beginCancellation(reason);
      return;
    }
    if (calibrationRound_ && phase_ == Phase::MEASURING) {
      radioElapsedMs_ = uint32_t((esp_timer_get_time() - localStartUs_) / 1000);
      Serial.printf("[AN] Pair radio elapsed %.3f s; restoring mesh for reports\n", double(radioElapsedMs_) / 1000);
    }
    if (restoreRadio) endRadio();
    Serial.printf("[TIMING] Round %u maximum controller gap=%llu us\n",
                  round_, static_cast<unsigned long long>(maxServiceGapUs_));
    resultOK_ = ok;
    resultReason_ = reason;
    if (sniffRound_) {
      const String target = macString(target_);
      distances.to<JsonObject>();
      slotRSSI[target] = nullptr;
      distances[target] = nullptr;
      if (targetSample_.count) {
        const double average = double(targetSample_.sum) / targetSample_.count;
        slotRSSI[target] = average;
        const double metres = rssiToDistance(average, roundA_, roundN_);
        if (std::isfinite(metres)) distances[target] = metres;
      }
    } else {
      for (size_t i = 0; i < samples_.size(); ++i) {
        String key = String(static_cast<unsigned long>(i + 1));
        slotPacketCounts[i] = samples_[i].count;
        if (i != selfIndex_ && samples_[i].count > 0) {
          slotRSSI[key] = double(samples_[i].sum) / samples_[i].count;
        } else {
          slotRSSI[key] = nullptr;
        }
      }
      // Convert the averaged RSSI once, keeping retransmissions identical.
      distances.to<JsonObject>();
      for (size_t i = 0; i < table_.size(); ++i) {
        if (i == selfIndex_) continue;
        const String mac = macString(table_[i]);
        const String key = String(static_cast<unsigned long>(i + 1));
        if (slotRSSI[key].isNull()) {
          distances[mac] = nullptr;
        } else {
          const double metres = rssiToDistance(slotRSSI[key].as<double>(), roundA_, roundN_);
          if (std::isfinite(metres)) distances[mac] = metres;
          else distances[mac] = nullptr;
        }
      }
    }
    Serial.printf("[ROUND %u] %s; RSSI: ", round_, reason);
    serializeJson(slotRSSI, Serial);
    Serial.println();
    if (restoreRadio && !restartMesh_()) {
      phase_ = Phase::FAILED;
      mode_ = "mesh";
      Serial.println("[ROUND] Mesh restart failed; results preserved locally");
      return;
    }
    mode_ = "mesh";
    phaseSinceMs_ = millis();
    lastMessageMs_ = millis() - RETRY_MS;
    if (isRoot()) {
      phase_ = Phase::COLLECTING;
      JsonDocument report;
      makeReport(report);
      reports[String(static_cast<unsigned long>(selfIndex_ + 1))].set(report);
      storeDistanceReport(report, selfIndex_);
      reportReceived_[selfIndex_] = true;
    } else {
      phase_ = Phase::REPORTING;
    }
  }
  bool acceptReport(JsonDocument &doc, uint32_t from) {
    if (!isRoot() || !validEnvelope(doc, 0) ||
        (phase_ != Phase::COLLECTING && phase_ != Phase::IDLE &&
         phase_ != Phase::CHANNEL_WAIT)) return false;
    size_t reporter;
    if (!reporterSlot(doc, reporter) || reporter == selfIndex_ ||
        peerIds_[reporter] != from || !doc["RSSI"].is<JsonObject>() ||
        !doc["Samples"].is<JsonObject>() ||
        !doc["Distances"].is<JsonObject>() ||
        !doc["ReporterMAC"].is<String>() ||
        doc["ReporterMAC"].as<String>() != macString(table_[reporter]) ||
        !doc["A"].is<double>() || !std::isfinite(doc["A"].as<double>()) ||
        !doc["n"].is<double>() || !std::isfinite(doc["n"].as<double>()) ||
        doc["n"].as<double>() <= 0) return false;
    JsonObject rssi = doc["RSSI"].as<JsonObject>();
    JsonObject counts = doc["Samples"].as<JsonObject>();
    JsonObject receivedDistances = doc["Distances"].as<JsonObject>();
    const char *status = doc["Status"] | "";
    if (strcmp(status, "ok") != 0 && strcmp(status, "failed") != 0) return false;
    if (sniffRound_) {
      const String target = macString(target_);
      if (rssi.size() != 1 || counts.size() != 1 || receivedDistances.size() != 1 ||
          !validTargetReading(rssi[target], counts[target], receivedDistances[target],
                              doc["A"].as<double>(), doc["n"].as<double>())) return false;
      if (discoveryRound_) {
        if (!doc["ChannelSamples"].is<JsonObject>() || doc["ChannelSamples"].size() != 13)
          return false;
        uint64_t total = 0;
        for (unsigned channel = 1; channel <= 13; ++channel) {
          JsonVariant value = doc["ChannelSamples"][String(channel)];
          if (!value.is<uint32_t>()) return false;
          total += value.as<uint32_t>();
        }
        if (total != counts[target].as<uint32_t>()) return false;
      }
    } else {
      if (rssi.size() != table_.size() || counts.size() != table_.size() ||
          receivedDistances.size() != table_.size() - 1) return false;
      for (size_t i = 0; i < table_.size(); ++i) {
        String key = String(static_cast<unsigned long>(i + 1));
        if (rssi[key].isUnbound()) return false;
        if (!rssi[key].isNull() && (!rssi[key].is<double>() ||
            rssi[key].as<double>() < -127 || rssi[key].as<double>() > 0)) return false;
        if (!counts[key].is<uint32_t>()) return false;
        uint32_t count = counts[key];
        if ((count == 0) != rssi[key].isNull() ||
            (i == reporter && count != 0)) return false;
        if (i != reporter) {
          JsonVariant value = receivedDistances[macString(table_[i])];
          if (value.isUnbound()) return false;
          const double expected = rssi[key].isNull()
              ? std::numeric_limits<double>::quiet_NaN()
              : rssiToDistance(rssi[key].as<double>(), doc["A"], doc["n"]);
          if (std::isfinite(expected)) {
            if (!value.is<double>() || !std::isfinite(value.as<double>()) ||
                value.as<double>() <= 0 ||
                std::fabs(value.as<double>() - expected) > expected * 1e-5)
              return false;
          } else if (!value.isNull()) return false;
        }
      }
    }
    String reporterKey = String(static_cast<unsigned long>(reporter + 1));
    reports[reporterKey].set(doc);
    if (reports.overflowed()) return false;
    if (!reportReceived_[reporter] && !storeDistanceReport(doc, reporter)) return false;
    reportReceived_[reporter] = true;
    JsonDocument reply;
    envelope(reply, "mesh", "report_ack", 1);
    sendJson(reply, from);
    Serial.printf("[ROUND %u] Report stored for slot %u (%s)\n", round_,
                  static_cast<unsigned>(reporter + 1), status);
    return true;
  }

  static String macString(const Mac &mac) {
    char text[18];
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(text);
  }

  bool storeDistanceReport(JsonDocument &doc, size_t reporter) {
    const String source = macString(table_[reporter]);
    if (sniffRound_) {
      JsonDocument &targetReports = discoveryRound_ ? channelDiscoveryReports : sniffReports;
      targetReports[source].set(doc);
      if (targetReports.overflowed()) return false;
    }
    distanceReports[source].set(doc["Distances"]);
    if (distanceReports.overflowed()) return false;
    Serial.printf("[ROUND %u] Distances stored from %s (A=%.2f, n=%.2f)\n",
                  round_, source.c_str(), doc["A"].as<double>(), doc["n"].as<double>());
    return true;
  }

  void printDistanceSummary() {
    Serial.printf("\n[ROUND %u] Node distance results (metres)\n", round_);
    for (size_t reporter = 0; reporter < table_.size(); ++reporter) {
      String source = macString(table_[reporter]);
      JsonObjectConst readings = distanceReports[source].as<JsonObjectConst>();
      source.replace(':', ' ');
      Serial.printf("\n%s measures%s\n", source.c_str(),
                    reportReceived_[reporter] ? "" : " [NO REPORT]");
      if (sniffRound_) {
        String target = macString(target_);
        JsonVariantConst distance = readings[target];
        target.replace(':', ' ');
        char value[32];
        if (!reportReceived_[reporter]) snprintf(value, sizeof(value), "no report");
        else if (distance.isNull()) snprintf(value, sizeof(value), "unavailable");
        else snprintf(value, sizeof(value), "%.4f", distance.as<double>());
        Serial.println("+-------------------+----------------+");
        Serial.println("| Target MAC        | Distance (m)   |");
        Serial.println("+-------------------+----------------+");
        Serial.printf("| %s | %14s |\n", target.c_str(), value);
        Serial.println("+-------------------+----------------+");
        continue;
      }
      Serial.println("+--------+----------------+");
      Serial.println("| Target | Distance (m)   |");
      Serial.println("+--------+----------------+");
      for (size_t target = 0; target < table_.size(); ++target) {
        if (target == reporter) continue;
        JsonVariantConst distance = readings[macString(table_[target])];
        char value[32];
        if (!reportReceived_[reporter]) snprintf(value, sizeof(value), "no report");
        else if (distance.isNull()) snprintf(value, sizeof(value), "unavailable");
        else snprintf(value, sizeof(value), "%.4f", distance.as<double>());
        Serial.printf("| %02X     | %14s |\n", table_[target][5], value);
      }
      Serial.println("+--------+----------------+");
    }
    Serial.println();
  }

  static bool validTargetReading(JsonVariantConst rssi, JsonVariantConst count,
                                 JsonVariantConst distance, double modelA, double modelN) {
    if (rssi.isUnbound() || distance.isUnbound() || !count.is<uint32_t>()) return false;
    if (count.as<uint32_t>() == 0) return rssi.isNull() && distance.isNull();
    if (!rssi.is<double>() || !std::isfinite(rssi.as<double>()) ||
        rssi.as<double>() < -127 || rssi.as<double>() > 0) return false;
    const double expected = rssiToDistance(rssi.as<double>(), modelA, modelN);
    if (!std::isfinite(expected)) return distance.isNull();
    return distance.is<double>() && std::isfinite(distance.as<double>()) &&
           distance.as<double>() > 0 &&
           std::fabs(distance.as<double>() - expected) <= expected * 1e-5;
  }

  void completeCollection() {
    phase_ = Phase::IDLE;
    if (!discoveryRound_) {
      if (sniffRound_) {
        sniffCycleReports[String(captureCycle_)].set(sniffReports.as<JsonObjectConst>());
        if (sniffCycleReports.overflowed()) {
          Serial.println("[ERROR] Cannot retain sniff cycle reports; operation stopped");
          return;
        }
        size_t received = 0;
        for (bool present : reportReceived_) if (present) ++received;
        Serial.printf("[SNIFF] Capture cycle %u/%u: reports %u/%u\n",
                      captureCycle_, TimeSliceProtocol::SNIFF_CYCLES,
                      static_cast<unsigned>(received), static_cast<unsigned>(table_.size()));
        const uint8_t next = TimeSliceProtocol::nextSniffCycle(captureCycle_);
        if (next) {
          pendingCycle_ = next;
          pendingChannel_ = targetChannel_;
          phase_ = Phase::CHANNEL_WAIT;
          phaseSinceMs_ = millis();
          mode_ = "mesh";
          return;
        }
        printSniffAverage();
        Serial.println("[COMMAND] All sniff cycles complete. Enter conf or sniff <12-hex-MAC>");
        return;
      }
      if (calibrationRound_) {
        Serial.println("[AN] Pair capture complete; calibration will check the readings next");
      } else {
        printDistanceSummary();
        Serial.println("[COMMAND] Round complete. Enter conf, sniff <12-hex-MAC>, or an opt <board>");
      }
      return;
    }
    uint64_t best = 0;
    discoveredChannel = 0;
    for (unsigned channel = 1; channel <= 13; ++channel) {
      uint64_t count = 0;
      for (JsonPair report : channelDiscoveryReports.as<JsonObject>())
        count += report.value()["ChannelSamples"][String(channel)].as<uint32_t>();
      if (count) Serial.printf("[SNIFF] Target channel %u: %llu observations\n",
                              channel, static_cast<unsigned long long>(count));
      if (count > best) { best = count; discoveredChannel = channel; }
    }
    if (!discoveredChannel) {
      Serial.println("[ERROR] Target not heard during channel discovery. Check MAC, activity and range; retry sniff");
      return;
    }
    Serial.printf("[SNIFF] Detected channel %u for %s; redistributing over mesh before %u capture cycles\n",
                  discoveredChannel, macString(target_).c_str(), TimeSliceProtocol::SNIFF_CYCLES);
    pendingChannel_ = discoveredChannel;
    pendingCycle_ = 1;
    phase_ = Phase::CHANNEL_WAIT;
    phaseSinceMs_ = millis();
    mode_ = "mesh";
  }

  void printSniffAverage() {
    const String target = macString(target_);
    String displayTarget = target;
    displayTarget.replace(':', ' ');
    Serial.printf("\n[SNIFF] Average distance across %u capture cycles (%u seconds each), channel %u\n",
                  TimeSliceProtocol::SNIFF_CYCLES,
                  unsigned(TimeSliceProtocol::SNIFF_US / 1000000), targetChannel_);
    for (const Mac &station : table_) {
      const String reporter = macString(station);
      DistanceAverage average;
      for (unsigned cycle = 1; cycle <= TimeSliceProtocol::SNIFF_CYCLES; ++cycle) {
        JsonObjectConst report = sniffCycleReports[String(cycle)][reporter].as<JsonObjectConst>();
        JsonVariantConst value = report["Distances"][target];
        if (strcmp(report["Status"] | "", "ok") == 0 && value.is<double>())
          average.add(value.as<double>());
      }
      sniffAverageCounts[reporter] = average.count;
      if (average.count) sniffAverageDistances[reporter][target] = average.mean();
      else sniffAverageDistances[reporter][target] = nullptr;
      String displayReporter = reporter;
      displayReporter.replace(':', ' ');
      Serial.printf("\n%s measures (average, valid cycles %u/%u)\n",
                    displayReporter.c_str(), average.count, TimeSliceProtocol::SNIFF_CYCLES);
      char result[32];
      if (average.count) snprintf(result, sizeof(result), "%.4f", average.mean());
      else snprintf(result, sizeof(result), "unavailable");
      Serial.println("+-------------------+----------------+");
      Serial.println("| Target MAC        | Average (m)    |");
      Serial.println("+-------------------+----------------+");
      Serial.printf("| %s | %14s |\n", displayTarget.c_str(), result);
      Serial.println("+-------------------+----------------+");
    }
  }
};
