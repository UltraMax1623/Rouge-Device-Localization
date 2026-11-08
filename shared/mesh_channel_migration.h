#pragma once

#include <algorithm>

// Included after the mesh, measurement controller and calibration globals.
// Radio changes are made in update(), never in a painlessMesh callback.
class MeshChannelMigration {
 public:
  enum class Phase : uint8_t { Idle, Preparing, Announcing, SwitchPending, Verifying, Confirming };

  bool busy() const { return phase_ != Phase::Idle || scanning_; }
  uint8_t routerChannel() const { return routerChannel_; }
  const char *debugPhase() const {
    if (scanning_) return "scanning";
    switch (phase_) {
      case Phase::Idle: return "idle";
      case Phase::Preparing: return "preparing";
      case Phase::Announcing: return "announcing";
      case Phase::SwitchPending: return "switch-pending";
      case Phase::Verifying: return "verifying";
      case Phase::Confirming: return "confirming";
    }
    return "unknown";
  }
  uint8_t debugFailures() const { return failureCount_; }
  uint32_t debugRetryMs() const {
    return int32_t(retryAfterMs_ - millis()) > 0 ? retryAfterMs_ - millis() : 0;
  }
  void bridgeConfigured(bool configured) {
    bridgeConfigured_ = configured;
    if (configured) bridgeRequestedMs_ = millis();
  }

  void discoverAtBoot() {
#if ROOT_FIRMWARE
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    deviceLog("[CHANNEL] Boot scan for configured hotspot starting");
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
      if (phase_ == Phase::Idle)
        deviceLogf("[CHANNEL] Board %u received prepare %u -> %u", BOARD_NUMBER, old, target);
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
    const bool hotspotConnected = WiFi.status() == WL_CONNECTED && WiFi.SSID() == ROUTER_SSID;
    const int hotspotChannel = hotspotConnected ? WiFi.channel() : 0;
    const bool routerRejoining = bridgeConfigured_ && routerChannel_ == meshChannel &&
        now - bridgeRequestedMs_ < ROUTER_REJOIN_GRACE_MS;
    if (!scanning_ && meshStarted && meshChannel == routerChannel_ &&
        !timeSlicing.busy() && !calibrationMode.active() &&
        !routerRejoining && WiFi.status() == WL_CONNECTED && !hotspotConnected &&
        now - lastRouterRepairMs_ >= 10000) {
      lastRouterRepairMs_ = now;
      deviceLog("[CHANNEL] Root joined a mesh AP instead of hotspot; resetting router station scan");
      mesh.configureRouterStation(ROUTER_SSID, ROUTER_PASSWORD);
      bridgeConfigured(true);
    }
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
        mesh.configureRouterStation(ROUTER_SSID, ROUTER_PASSWORD);
        bridgeConfigured(true);
        deviceLogf("[CHANNEL] Router connection requested on mesh channel %u", meshChannel);
      }
      if (meshStarted && hotspotChannel >= 1 && hotspotChannel <= 13 && hotspotChannel != meshChannel) {
        if (timeSlicing.busy() || calibrationMode.active()) return;
        routerChannel_ = uint8_t(hotspotChannel);
        deviceLogf("[CHANNEL] Hotspot moved to %u; restoring mesh on %u before coordinated migration",
                   unsigned(routerChannel_), unsigned(meshChannel));
        switchTo(meshChannel);
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
                 !routerRejoining && now - lastScanMs_ >= 60000 && !hotspotConnected) {
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
        phaseSinceMs_ = millis();
        return; // Read channel and association again after restarting the radio.
      }
    }
    if ((phase_ == Phase::Verifying || phase_ == Phase::Confirming) &&
        hotspotChannel >= 1 && hotspotChannel <= 13 && hotspotChannel != target_) {
      routerChannel_ = uint8_t(hotspotChannel);
      deviceLogf("[CHANNEL] Hotspot moved to %u during migration to %u", unsigned(routerChannel_), unsigned(target_));
      rollback("hotspot channel changed");
      return;
    }
    if (phase_ == Phase::Verifying) {
      if (meshStarted && mesh.getNodeList().size() >= TEST_BOARD_COUNT - 1 &&
          allPeersRejoined() && hotspotChannel == target_) {
        phase_ = Phase::Confirming;
        phaseSinceMs_ = now;
        lastSendMs_ = now - 500;
        for (unsigned i = 2; i <= TEST_BOARD_COUNT; ++i) acknowledged_[i] = false;
        deviceLogf("[CHANNEL] Router and %u peers rejoined on channel %u",
                   TEST_BOARD_COUNT - 1, target_);
      } else if (now - phaseSinceMs_ >= ROOT_VERIFY_MS) {
        rollback(!hotspotConnected ? "root did not join hotspot within 30 seconds"
                                   : "mesh peers did not rejoin within 30 seconds");
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
      lastRootChannel_ = meshChannel;
      searching_ = false;
    }
    if (phase_ == Phase::Preparing && now - phaseSinceMs_ > 25000) {
      phase_ = Phase::Idle;
      deviceLog("[CHANNEL] Commit missing; kept old mesh channel");
    }
    if (phase_ == Phase::SwitchPending && int32_t(now - switchAtMs_) >= 0) {
      if (!switchTo(target_)) { rollback("mesh restart failed"); return; }
      phase_ = Phase::Verifying;
      phaseSinceMs_ = millis();
      return;
    }
    if (phase_ == Phase::Verifying &&
        (now - phaseSinceMs_ >= ROOT_VERIFY_MS + 20000 ||
         (!rootReachable && now - phaseSinceMs_ >= ROOT_VERIFY_MS + 5000 &&
          now - lastRootSeenMs_ >= 5000))) {
      rollback("root confirmation missing");
      lastSearchMs_ = millis();
      searching_ = true; // Hold the old rendezvous channel before moving again.
      return;
    }
#if NORMAL_CHANNEL_DEBUG
    if (phase_ == Phase::Verifying && now - lastTraceMs_ >= 5000) {
      lastTraceMs_ = now;
      deviceLogf("[CHANNEL] Board %u verifying: configured=%u radio=%d peers=%u root=%s",
                 BOARD_NUMBER, meshChannel, WiFi.channel(),
                 unsigned(mesh.getNodeList().size()), rootReachable ? "seen" : "missing");
    }
#endif
    // A rebooted node starts on the bootstrap channel; search only after
    // prolonged absence of the root and stop immediately on reconnection.
    if (phase_ == Phase::Idle && !rootReachable && !timeSlicing.busy() &&
        !calibrationMode.active() && now - lastRootSeenMs_ >= ROOT_SEARCH_DWELL_MS &&
        (!searching_ || now - lastSearchMs_ >= ROOT_SEARCH_DWELL_MS)) {
      wifi_country_t country{};
      if (esp_wifi_get_country(&country) != ESP_OK || country.schan < 1 || country.nchan < 1) return;
      const unsigned first = country.schan;
      const unsigned last = std::min(13u, first + country.nchan - 1);
      if (first > last) return;
      // When already on the last known channel, the initial wait was its dwell.
      const bool tryKnown = !searching_ && lastRootChannel_ != meshChannel &&
          lastRootChannel_ >= first && lastRootChannel_ <= last;
      const uint8_t next = tryKnown ? lastRootChannel_ :
          (meshChannel < first || meshChannel >= last ? first : meshChannel + 1);
      searching_ = true;
      lastSearchMs_ = now;
      deviceLogf("[CHANNEL] Searching for Root 01 on channel %u (25 s dwell)", unsigned(next));
      if (next != meshChannel) switchTo(next);
    }
#endif
  }

 private:
  static constexpr uint32_t ROOT_VERIFY_MS = 30000;
  static constexpr uint32_t ROUTER_REJOIN_GRACE_MS = 20000;
  static constexpr uint32_t ROOT_SEARCH_DWELL_MS = 25000;
  Phase phase_ = Phase::Idle;
  uint8_t target_ = 0, oldChannel_ = 0, routerChannel_ = 0, failedChannel_ = 0;
  uint32_t epoch_ = 0, phaseSinceMs_ = 0, switchAtMs_ = 0, lastSendMs_ = 0;
  uint32_t lastScanMs_ = 0, scanStartedMs_ = 0, lastRootSeenMs_ = 0, lastSearchMs_ = 0;
#if !ROOT_FIRMWARE && NORMAL_CHANNEL_DEBUG
  uint32_t lastTraceMs_ = 0;
#endif
  bool scanning_ = false, searching_ = false;
  uint8_t lastRootChannel_ = 0;
  bool bridgeConfigured_ = false;
  uint32_t bridgeRequestedMs_ = 0;
  uint32_t retryAfterMs_ = 0;
  uint8_t failureCount_ = 0;
  uint32_t lastRouterRepairMs_ = 0;
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
    if (meshStarted) stopMeshForMeasurement();
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
