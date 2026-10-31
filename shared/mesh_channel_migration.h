#pragma once

#include <algorithm>

// Included after the mesh, measurement controller and calibration globals.
// Radio changes are made in update(), never in a painlessMesh callback.
class MeshChannelMigration {
 public:
  enum class Phase : uint8_t { Idle, Preparing, Announcing, SwitchPending, Verifying, Confirming };

  bool busy() const { return phase_ != Phase::Idle || scanning_; }
  uint8_t routerChannel() const { return routerChannel_; }
  void bridgeConfigured(bool configured) { bridgeConfigured_ = configured; }

  void discoverAtBoot() {
#if ROOT_FIRMWARE
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    const int count = WiFi.scanNetworks();
    acceptScan(count);
    WiFi.disconnect(false, false);
    lastScanMs_ = millis();
#endif
  }

  bool handle(JsonDocument &doc, uint32_t from) {
    const char *action = doc["Action"] | "";
    if (!doc["Epoch"].is<uint32_t>() || !doc["Channel"].is<unsigned>() ||
        !doc["OldChannel"].is<unsigned>()) return false;
    const uint32_t epoch = doc["Epoch"].as<uint32_t>();
    const unsigned target = doc["Channel"].as<unsigned>();
    const unsigned old = doc["OldChannel"].as<unsigned>();
    if (!epoch || target < 1 || target > 13 || old < 1 || old > 13 || target == old)
      return false;

#if ROOT_FIRMWARE
    const bool preparationAck = strcmp(action, "channel_ack") == 0 &&
                                phase_ == Phase::Preparing;
    const bool confirmationAck = strcmp(action, "channel_confirm_ack") == 0 &&
                                 phase_ == Phase::Confirming;
    if ((!preparationAck && !confirmationAck) ||
        epoch != epoch_ || target != target_ || old != oldChannel_ ||
        !doc["Board"].is<unsigned>()) return false;
    const unsigned board = doc["Board"].as<unsigned>();
    if (board < 2 || board > TEST_BOARD_COUNT) return false;
    bool connected = false;
    for (uint32_t peer : mesh.getNodeList()) if (peer == from) connected = true;
    if (!connected) return false;
    for (unsigned i = 2; i <= TEST_BOARD_COUNT; ++i)
      if (i != board && acknowledged_[i] && peerIds_[i] == from) return false;
    acknowledged_[board] = true;
    peerIds_[board] = from;
    deviceLogf("[CHANNEL] Board %u acknowledged %s channel %u", board,
               preparationAck ? "planned" : "active", target_);
    return true;
#else
    if (from != timeSlicing.rootNodeId()) return false;
    if (strcmp(action, "channel_prepare") == 0) {
      if (target == meshChannel || old != meshChannel || timeSlicing.busy() ||
          calibrationMode.active()) return false;
      if (phase_ != Phase::Idle &&
          (phase_ != Phase::Preparing || epoch_ != epoch || target_ != target)) return false;
      epoch_ = epoch;
      target_ = uint8_t(target);
      oldChannel_ = uint8_t(old);
      phase_ = Phase::Preparing;
      phaseSinceMs_ = millis();
      send("channel_ack", true);
      return true;
    }
    if (epoch != epoch_ || target != target_ || old != oldChannel_) return false;
    if (strcmp(action, "channel_commit") == 0 && phase_ == Phase::Preparing) {
      phase_ = Phase::SwitchPending;
      switchAtMs_ = millis() + 3000;
      deviceLogf("[CHANNEL] Root committed %u -> %u; switching shortly", old, target);
      return true;
    }
    if (strcmp(action, "channel_confirm") == 0 && meshChannel == target_ &&
        (phase_ == Phase::Verifying || phase_ == Phase::Idle)) {
      if (phase_ == Phase::Verifying) {
        phase_ = Phase::Idle;
        deviceLogf("[CHANNEL] Root confirmed mesh channel %u", meshChannel);
      }
      send("channel_confirm_ack", true);
      return true;
    }
    return false;
#endif
  }

  void update() {
    const uint32_t now = millis();
#if ROOT_FIRMWARE
    if (scanning_) {
      const int result = WiFi.scanComplete();
      if (result >= 0) { scanning_ = false; acceptScan(result); }
      else if (now - scanStartedMs_ > 12000) {
        WiFi.scanDelete();
        scanning_ = false;
        deviceLog("[CHANNEL] Router scan timed out; keeping current mesh channel");
      }
      return;
    }
    if (phase_ == Phase::Idle) {
      if (meshStarted && routerChannel_ == meshChannel && !bridgeConfigured_) {
        mesh.stationManual(ROUTER_SSID, ROUTER_PASSWORD);
        bridgeConfigured_ = true;
        deviceLogf("[CHANNEL] Router connection requested on mesh channel %u", meshChannel);
      }
      if (WiFi.status() == WL_CONNECTED && WiFi.channel() >= 1 &&
          WiFi.channel() <= 13 && WiFi.channel() != meshChannel) {
        const uint8_t movedChannel = WiFi.channel();
        if (timeSlicing.busy() || calibrationMode.active() ||
            (movedChannel == failedChannel_ &&
             (failureCount_ >= 3 || int32_t(now - retryAfterMs_) < 0))) return;
        routerChannel_ = movedChannel;
        deviceLogf("[CHANNEL] Router moved the radio to %u; restarting mesh there", movedChannel);
        if (switchTo(movedChannel)) {
          failedChannel_ = 0;
          failureCount_ = 0;
        } else {
          target_ = movedChannel;
          markFailure();
          deviceLog("[CHANNEL] Could not follow router channel; bootstrap mesh retained");
        }
        return;
      }
      if (routerChannel_ && routerChannel_ != meshChannel &&
          (routerChannel_ != failedChannel_ ||
           (failureCount_ < 3 && int32_t(now - retryAfterMs_) >= 0)) &&
          readyToMigrate()) {
        target_ = routerChannel_;
        oldChannel_ = meshChannel;
        epoch_ = esp_random();
        if (!epoch_) epoch_ = 1;
        for (unsigned i = 0; i < 16; ++i) { acknowledged_[i] = false; peerIds_[i] = 0; }
        phase_ = Phase::Preparing;
        phaseSinceMs_ = now;
        lastSendMs_ = now - 500;
        deviceLogf("[CHANNEL] Preparing mesh migration %u -> %u", oldChannel_, target_);
      } else if (meshStarted && !timeSlicing.busy() && !calibrationMode.active() &&
                 now - lastScanMs_ >= 60000 &&
                 (routerChannel_ == 0 || WiFi.status() != WL_CONNECTED)) {
        lastScanMs_ = now;
        if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING) {
          scanning_ = true;
          scanStartedMs_ = now;
          deviceLog("[CHANNEL] Scanning for Pavan router channel");
        } else {
          const int result = WiFi.scanComplete();
          if (result >= 0) acceptScan(result);
        }
      }
      return;
    }
    if (phase_ == Phase::Preparing) {
      if (allAcknowledged()) {
        phase_ = Phase::Announcing;
        switchAtMs_ = now + 4000;
        lastSendMs_ = now - 500;
        deviceLogf("[CHANNEL] All boards ready; committing channel %u", target_);
      } else if (now - phaseSinceMs_ > 20000) {
        markFailure();
        phase_ = Phase::Idle;
        deviceLog("[CHANNEL] Prepare timed out; mesh stays on old channel");
        return;
      } else if (now - lastSendMs_ >= 500) {
        send("channel_prepare");
        lastSendMs_ = now;
      }
    }
    if (phase_ == Phase::Announcing) {
      if (now - lastSendMs_ >= 500 && int32_t(switchAtMs_ - now) > 1000) {
        send("channel_commit");
        lastSendMs_ = now;
      }
      if (int32_t(now - switchAtMs_) >= 0) {
        if (!switchTo(target_)) { rollback("mesh restart failed"); return; }
        phase_ = Phase::Verifying;
        phaseSinceMs_ = now;
      }
    }
    if (phase_ == Phase::Verifying) {
      if (meshStarted && mesh.getNodeList().size() >= TEST_BOARD_COUNT - 1 &&
          allPeersRejoined() &&
          WiFi.status() == WL_CONNECTED && WiFi.channel() == target_) {
        phase_ = Phase::Confirming;
        phaseSinceMs_ = now;
        lastSendMs_ = now - 500;
        for (unsigned i = 2; i <= TEST_BOARD_COUNT; ++i) acknowledged_[i] = false;
        deviceLogf("[CHANNEL] Router and %u peers rejoined on channel %u",
                   TEST_BOARD_COUNT - 1, target_);
      } else if (now - phaseSinceMs_ > 30000) {
        rollback("router or peers did not rejoin");
      }
    }
    if (phase_ == Phase::Confirming) {
      if (now - lastSendMs_ >= 500) { send("channel_confirm"); lastSendMs_ = now; }
      if (allAcknowledged()) {
        phase_ = Phase::Idle;
        failedChannel_ = 0;
        failureCount_ = 0;
        deviceLogf("[CHANNEL] Migration complete: mesh and router channel %u", meshChannel);
      } else if (now - phaseSinceMs_ > 10000) {
        rollback("node confirmation missing");
      }
    }
#else
    bool rootReachable = false;
    if (meshStarted) for (uint32_t peer : mesh.getNodeList())
      if (peer == timeSlicing.rootNodeId()) rootReachable = true;
    if (rootReachable) {
      lastRootSeenMs_ = now;
      searching_ = false;
    }
    if (phase_ == Phase::Preparing && now - phaseSinceMs_ > 25000) {
      phase_ = Phase::Idle;
      deviceLog("[CHANNEL] Commit missing; kept old mesh channel");
    }
    if (phase_ == Phase::SwitchPending && int32_t(now - switchAtMs_) >= 0) {
      if (!switchTo(target_)) { rollback("mesh restart failed"); return; }
      phase_ = Phase::Verifying;
      phaseSinceMs_ = now;
    }
    if (phase_ == Phase::Verifying && now - phaseSinceMs_ > 30000)
      rollback("root confirmation missing");
    // A rebooted node starts on the bootstrap channel; search only after
    // prolonged absence of the root and stop immediately on reconnection.
    if (phase_ == Phase::Idle && !rootReachable && !timeSlicing.busy() &&
        !calibrationMode.active() && now - lastRootSeenMs_ > 20000 &&
        (!searching_ || now - lastSearchMs_ > 8000)) {
      searching_ = true;
      lastSearchMs_ = now;
      const uint8_t next = meshChannel == 13 ? 1 : meshChannel + 1;
      deviceLogf("[CHANNEL] Searching for root on channel %u", next);
      switchTo(next);
    }
#endif
  }

 private:
  Phase phase_ = Phase::Idle;
  uint8_t target_ = 0, oldChannel_ = 0, routerChannel_ = 0, failedChannel_ = 0;
  uint32_t epoch_ = 0, phaseSinceMs_ = 0, switchAtMs_ = 0, lastSendMs_ = 0;
  uint32_t lastScanMs_ = 0, scanStartedMs_ = 0, lastRootSeenMs_ = 0, lastSearchMs_ = 0;
  bool scanning_ = false, searching_ = false;
  bool bridgeConfigured_ = false;
  uint32_t retryAfterMs_ = 0;
  uint8_t failureCount_ = 0;
#if ROOT_FIRMWARE
  bool acknowledged_[16]{};
  uint32_t peerIds_[16]{};
#endif

  bool readyToMigrate() const {
#if ROOT_FIRMWARE
    return meshStarted && !timeSlicing.busy() && !calibrationMode.active() &&
           calibrationMode.networkSynced() &&
           timeSlicing.networkReadyForConfiguration(TEST_BOARD_COUNT);
#else
    return false;
#endif
  }
  bool allAcknowledged() const {
#if ROOT_FIRMWARE
    for (unsigned board = 2; board <= TEST_BOARD_COUNT; ++board)
      if (!acknowledged_[board]) return false;
    return true;
#else
    return false;
#endif
  }
  bool allPeersRejoined() const {
#if ROOT_FIRMWARE
    const auto peers = mesh.getNodeList();
    for (unsigned board = 2; board <= TEST_BOARD_COUNT; ++board)
      if (std::find(peers.begin(), peers.end(), peerIds_[board]) == peers.end())
        return false;
    return true;
#else
    return false;
#endif
  }
  void send(const char *action, bool single = false) {
    JsonDocument doc;
    doc["Mode"] = "mesh";
    doc["Action"] = action;
    doc["Epoch"] = epoch_;
    doc["Channel"] = target_;
    doc["OldChannel"] = oldChannel_;
    if (single) doc["Board"] = BOARD_NUMBER;
    String payload;
    serializeJson(doc, payload);
    if (single) mesh.sendSingle(timeSlicing.rootNodeId(), payload);
    else mesh.sendBroadcast(payload);
  }
  bool switchTo(uint8_t channel) {
    if (channel < 1 || channel > 13 || timeSlicing.busy() || calibrationMode.active()) return false;
    const uint8_t previous = meshChannel;
    stopMeshForMeasurement();
    meshChannel = channel;
    if (restartMesh()) {
      deviceLogf("[CHANNEL] Mesh restarted on channel %u", channel);
      return true;
    }
    meshChannel = previous;
    if (!restartMesh()) deviceLog("[CHANNEL] Rollback restart failed; mesh recovery will retry");
    return false;
  }
  void rollback(const char *reason) {
    const uint8_t old = oldChannel_;
    const uint8_t failed = target_;
    phase_ = Phase::Idle;
#if ROOT_FIRMWARE
    markFailure();
#endif
    if (meshChannel != old) switchTo(old);
    deviceLogf("[CHANNEL] Migration to %u failed: %s; reverted to %u",
               failed, reason, meshChannel);
  }
#if ROOT_FIRMWARE
  void markFailure() {
    failedChannel_ = target_;
    if (failureCount_ < 3) ++failureCount_;
    retryAfterMs_ = millis() + 120000;
  }
  void acceptScan(int count) {
    int bestRssi = -1000;
    uint8_t found = 0;
    for (int i = 0; i < count; ++i) {
      const int channel = WiFi.channel(i);
      if (WiFi.SSID(i) == ROUTER_SSID && channel >= 1 && channel <= 13 &&
          WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        found = uint8_t(channel);
      }
    }
    WiFi.scanDelete();
    if (found) {
      if (found != routerChannel_) { failedChannel_ = 0; failureCount_ = 0; }
      routerChannel_ = found;
      deviceLogf("[CHANNEL] Pavan detected on channel %u; mesh channel %u",
                 routerChannel_, meshChannel);
    } else deviceLog("[CHANNEL] Pavan not found; staying on current mesh channel");
  }
#endif
};
