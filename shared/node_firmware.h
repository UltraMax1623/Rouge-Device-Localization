#pragma once

//imports
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "painlessMesh.h"
#include <ArduinoJson.h>
#include <array>
#include <vector>
#include <utility>
#include "../root_node/time_slicing.h"
#include "mode_indicator.h"
#include "serial_commands.h"


//Defines

#define MESH_PREFIX     "MeshNetwork"
#define MESH_PASSWORD   "pavansqaured"
#define MESH_PORT       5555

//global const
// Fallbacks only: setup loads saved calibration; an opt + save replaces the
// active globals and distributes/persists the model across the mesh.
double A = -60.0;
double n = 2.0;
uint8_t target_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
#if ROOT_FIRMWARE
uint8_t AP_self_mac[6] = {0x54, 0x44, 0x33, 0x22, 0x11, 0x01};
#else
// Filled from the factory interface identity; no custom AP address on nodes.
uint8_t AP_self_mac[6] = {};
#endif
uint8_t station_self_mac[6] = {0x44, 0x33, 0x22, 0x11, 0x00, BOARD_NUMBER};
const uint8_t root_AP_mac[6] = {0x54, 0x44, 0x33, 0x22, 0x11, 0x01};
bool meshStarted = false;
int packet_count =  0;
int RSSI = 0;
Scheduler userScheduler;
MeasurementMesh mesh;
JsonDocument meshmess;
using MacAddress = std::array<uint8_t, 6>;
std::vector<MacAddress> macList;
String currentMode = "mesh";

#if ROOT_FIRMWARE
const char* ROUTER_SSID = "Pavan";
const char* ROUTER_PASSWORD = "12345678";

const char* BACKEND_HOST = "10.131.226.153";  // Replace with your PC's LAN IP
const uint16_t BACKEND_PORT = 3000;
const char* DEVICE_TOKEN = "local-device-token-change-me";

WebSocketsClient webSocket;

String uiNodeState[16];
bool uiSnapshotPending = false;

bool uiConfInProgress = false;

void publishNodeState(uint8_t board, const char* status) {
  if (board >= 16) return;

  char nodeId[4];
  snprintf(nodeId, sizeof(nodeId), "%02u", static_cast<unsigned>(board));

  JsonDocument message;
  message["type"] = "node_status";
  message["nodeId"] = nodeId;
  message["status"] = status;

  serializeJson(message, uiNodeState[board]);

  if (webSocket.isConnected()) {
    webSocket.sendTXT(uiNodeState[board]);
  }
}

constexpr uint8_t UI_LOG_QUEUE_SIZE = 20;
constexpr size_t UI_LOG_MESSAGE_LENGTH = 160;

char uiLogQueue[UI_LOG_QUEUE_SIZE][UI_LOG_MESSAGE_LENGTH];
uint8_t uiLogHead = 0;
uint8_t uiLogCount = 0;

void queueDeviceLog(const char* text) {
  if (!text) return;

  // If full, discard the oldest entry so new errors can still be queued.
  if (uiLogCount == UI_LOG_QUEUE_SIZE) {
    uiLogHead = (uiLogHead + 1) % UI_LOG_QUEUE_SIZE;
    --uiLogCount;
  }

  uint8_t tail = (uiLogHead + uiLogCount) % UI_LOG_QUEUE_SIZE;
  strncpy(uiLogQueue[tail], text, UI_LOG_MESSAGE_LENGTH - 1);
  uiLogQueue[tail][UI_LOG_MESSAGE_LENGTH - 1] = '\0';
  ++uiLogCount;
}

void deviceLog(const char* text) {
  Serial.println(text);
  queueDeviceLog(text);
}

void deviceLogf(const char* format, ...) {
  char line[UI_LOG_MESSAGE_LENGTH];

  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  // Remove trailing newline characters so the webpage displays one clean line.
  size_t length = strlen(line);
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
    line[--length] = '\0';
  }

  Serial.println(line);
  queueDeviceLog(line);
}

void flushDeviceLogQueue() {
  if (!webSocket.isConnected()) return;

  // Send only a few each loop so logging doesn't monopolize the loop.
  for (uint8_t sent = 0; sent < 3 && uiLogCount > 0; ++sent) {
    JsonDocument message;
    message["type"] = "log";
    message["source"] = "ESP32";
    message["message"] = uiLogQueue[uiLogHead];

    String payload;
    serializeJson(message, payload);

    if (!webSocket.sendTXT(payload)) return;

    uiLogHead = (uiLogHead + 1) % UI_LOG_QUEUE_SIZE;
    --uiLogCount;
  }
}

char wsCommandBuffer[64] = {};
bool wsCommandPending = false;

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
    uiSnapshotPending = true;
      deviceLog("[WS] Connected to backend");
      break;

    case WStype_DISCONNECTED:
      deviceLog("[WS] Disconnected from backend");
      break;

    case WStype_TEXT:
  if (length == 4 && memcmp(payload, "conf", 4) == 0) {
    memcpy(wsCommandBuffer, "conf", 5);
    wsCommandPending = true;
    deviceLog("[WS] Queued conf command");
  } else {
    deviceLog("[WS] Rejected unsupported command");
  }
  break;

    case WStype_ERROR:
      deviceLog("[WS] Error");
      break;

    default:
      break;
  }
}
#else
// Normal nodes keep printing locally; only the root has a backend WebSocket.
#define deviceLog(text) Serial.println(text)
#define deviceLogf(...) Serial.printf(__VA_ARGS__)
#endif

//funtions
bool node_mssg_parser(const String &message, uint32_t from);
void receivedCallback(uint32_t from, String &message);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
void nodeTimeAdjustedCallback(int32_t offset);
bool configureSelfMacs();
bool restartMesh();
void stopMeshForMeasurement();
void runTimeSlicing(bool armOnly = false);

TimeSlicingController timeSlicing(mesh, AP_self_mac, station_self_mac,
    root_AP_mac, meshStarted, currentMode, stopMeshForMeasurement, restartMesh);
// One-based JSON keys follow macList order. Counts remain zero-based in C++.
JsonDocument &slotRSSI = timeSlicing.slotRSSI;
std::vector<uint32_t> &slotPacketCounts = timeSlicing.slotPacketCounts;
JsonDocument &roundReports = timeSlicing.reports;
JsonDocument &macDistances = timeSlicing.distances;
JsonDocument &roundDistanceReports = timeSlicing.distanceReports;
uint32_t &distanceReportRoundId = timeSlicing.distanceRoundId;
JsonDocument &sniffReports = timeSlicing.sniffReports;
JsonDocument &sniffCycleReports = timeSlicing.sniffCycleReports;
JsonDocument &sniffAverageDistances = timeSlicing.sniffAverageDistances;
JsonDocument &sniffAverageCounts = timeSlicing.sniffAverageCounts;
JsonDocument &channelDiscoveryReports = timeSlicing.channelDiscoveryReports;
uint8_t &true_channel = timeSlicing.discoveredChannel;

#include "calibration_mode.h"
CalibrationMode calibrationMode;

// Root command entry point; normal nodes receive configuration over mesh.
bool runConfig() {
  return timeSlicing.beginConfiguration(macList);
}

bool handleConfigMode(JsonDocument &doc, uint32_t from){
  if (!calibrationMode.allowConfiguration(doc)) return false;
  // Other modes will be handled separately later.
  const char *mode = doc["Mode"] | "";

  if (strcmp(mode, "conf") != 0) {
    return false;
  }

  // Configuration must be marked root -> nodes.
  if (!doc["RootDirection"].is<int>() ||
      doc["RootDirection"].as<int>() != 1) {
    deviceLog("Configuration rejected: invalid direction");
    return false;
  }

  if (!doc["MacList"].is<JsonObject>()) {
    deviceLog("Configuration rejected: missing/invalid MacList");
    return false;
  }

  JsonObject receivedList = doc["MacList"].as<JsonObject>();

  // Temporary list prevents partially applying an invalid configuration.
  std::vector<MacAddress> newMacList;
  newMacList.reserve(receivedList.size());

  for (size_t i = 0; i < receivedList.size(); ++i) {
    String key = String(static_cast<unsigned long>(i + 1));

    // Keys must be consecutive: "1", "2", ... "N".
    if (!receivedList[key].is<JsonArray>()) {
      deviceLogf("Configuration rejected: invalid entry %s\n",
                    key.c_str());
      return false;
    }

    JsonArray bytes = receivedList[key].as<JsonArray>();

    if (bytes.size() != 6) {
      deviceLog("Configuration rejected: MAC needs six bytes");
      return false;
    }

    MacAddress mac{};

    for (size_t j = 0; j < mac.size(); ++j) {
      if (!bytes[j].is<unsigned int>() ||
          bytes[j].as<unsigned int>() > 255) {
        deviceLog("Configuration rejected: invalid MAC byte");
        return false;
      }

      mac[j] = static_cast<uint8_t>(bytes[j].as<unsigned int>());
    }

    for (const MacAddress &existing : newMacList) {
      if (existing == mac) {
        deviceLog("Configuration rejected: duplicate MAC");
        return false;
      }
    }

    newMacList.push_back(mac);
  }

  // The controller also verifies the root, round ID, and timing parameters.
  if (!timeSlicing.acceptConfiguration(doc, newMacList, from)) {
    deviceLog("Configuration rejected: root, round, or station list invalid");
    return false;
  }

  // Apply only after every entry has passed validation.
  macList = std::move(newMacList);
  runTimeSlicing(true);  // Arm only; radio changes happen later in loop().

  deviceLogf("Configuration stored: %u MACs\n",
                static_cast<unsigned>(macList.size()));

  return true;
}
bool handleMeshMode(JsonDocument &doc) {
  // TODO: Implement mesh mode later.
  return false;
}

bool handleSniffMode(JsonDocument &doc, uint32_t from) {
  return timeSlicing.handleControl(doc, from);
}

bool node_mssg_parser(const String &message, uint32_t from) {
  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    deviceLogf("JSON error: %s\n", error.c_str());
    return false;
  }

  const char *mode = doc["Mode"] | "";
  const char *action = doc["Action"] | "";

  if (strcmp(mode, "an") == 0) return calibrationMode.handle(doc, from);
  deviceLogf("Message from %u\n", from);

  if (strcmp(mode, "conf") == 0) {
    if (strcmp(action, "prepare") == 0) return handleConfigMode(doc, from);
    return timeSlicing.handleControl(doc, from);
  }

  if (strcmp(mode, "mesh") == 0) {
    if (strcmp(action, "rssi_report") == 0 || strcmp(action, "sniff_report") == 0 ||
        strcmp(action, "report_ack") == 0)
      return timeSlicing.handleControl(doc, from);
    return handleMeshMode(doc);
  }

  if (strcmp(mode, "sniff") == 0) {
    return handleSniffMode(doc, from);
  }

  deviceLog("Message rejected: missing or unknown Mode");
  return false;
}

void receivedCallback(uint32_t from, String &message) {
  node_mssg_parser(message, from);
}

void newConnectionCallback(uint32_t nodeId) {
  timeSlicing.topologyChanged();
  calibrationMode.topologyChanged();
  deviceLogf("Connected: %u\n", nodeId);
}

void changedConnectionCallback() {
  timeSlicing.topologyChanged();
  calibrationMode.topologyChanged();
  deviceLog("Mesh connections changed");
}

void nodeTimeAdjustedCallback(int32_t offset) {
  timeSlicing.timeAdjusted(offset);
  deviceLogf("Mesh time adjusted: %ld\n", static_cast<long>(offset));
}

bool configureSelfMacs() {
#if !ROOT_FIRMWARE
  if (esp_read_mac(AP_self_mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    deviceLog("Cannot read factory AP identity");
    return false;
  }
#endif
  if (memcmp(AP_self_mac, station_self_mac, 6) == 0 ||
      (AP_self_mac[0] & 1) || (station_self_mac[0] & 1)) {
    deviceLog("Invalid MACs: use distinct unicast addresses");
    return false;
  }

  // Core 3.x painlessMesh derives its ID using esp_read_mac(). Register the
  // custom addresses there as well as in the Wi-Fi driver.
#if ROOT_FIRMWARE
  if (esp_iface_mac_addr_set(AP_self_mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    deviceLog("Custom root AP registration failed");
    return false;
  }
#endif
  if (esp_iface_mac_addr_set(station_self_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    deviceLog("Custom interface MAC registration failed");
    return false;
  }

  // Initialize the Arduino Wi-Fi driver, then stop the interfaces before
  // changing their addresses. Restart without deinitializing the driver.
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_AP_STA)) {
    deviceLog("Wi-Fi initialization failed");
    return false;
  }

  // Measurement keeps the driver initialized. Its MACs normally survive
  // STA <-> AP/STA changes; avoid stopping/restarting Wi-Fi just to set
  // addresses which are already correct.
  uint8_t currentAP[6], currentStation[6];
  if (esp_wifi_get_mac(WIFI_IF_AP, currentAP) == ESP_OK &&
      esp_wifi_get_mac(WIFI_IF_STA, currentStation) == ESP_OK &&
      memcmp(currentAP, AP_self_mac, 6) == 0 &&
      memcmp(currentStation, station_self_mac, 6) == 0) return true;

  esp_err_t error = esp_wifi_stop();
  if (error != ESP_OK) {
    deviceLogf("Wi-Fi stop failed: %s\n", esp_err_to_name(error));
    return false;
  }

#if ROOT_FIRMWARE
  error = esp_wifi_set_mac(WIFI_IF_AP, AP_self_mac);
  if (error != ESP_OK) {
    deviceLogf("AP MAC setup failed: %s\n", esp_err_to_name(error));
    return false;
  }

#endif

  error = esp_wifi_set_mac(WIFI_IF_STA, station_self_mac);
  if (error != ESP_OK) {
    deviceLogf("Station MAC setup failed: %s\n", esp_err_to_name(error));
    return false;
  }

  error = esp_wifi_start();
  if (error != ESP_OK) {
    deviceLogf("Wi-Fi start failed: %s\n", esp_err_to_name(error));
    return false;
  }
  return true;
}

bool restartMesh() {
  meshStarted = false;
  if (!configureSelfMacs()) {
    deviceLog("Mesh not started: check custom MAC configuration");
    return false;
  }

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT,
            WIFI_AP_STA, TimeSliceProtocol::CHANNEL);
            #if ROOT_FIRMWARE
  mesh.stationManual(ROUTER_SSID, ROUTER_PASSWORD);
#endif
  mesh.setRoot(timeSlicing.isRoot());
  mesh.setContainsRoot(true);

  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  uint8_t actualAP[6];
  uint8_t actualStation[6];
  if (esp_wifi_get_mac(WIFI_IF_AP, actualAP) != ESP_OK ||
      esp_wifi_get_mac(WIFI_IF_STA, actualStation) != ESP_OK ||
      memcmp(actualAP, AP_self_mac, 6) != 0 ||
      memcmp(actualStation, station_self_mac, 6) != 0 ||
      mesh.getNodeId() != painlessmesh::tcp::encodeNodeId(AP_self_mac)) {
    deviceLog("Custom MAC verification failed; stopping mesh");
    mesh.stopForMeasurement();
    return false;
  }

  meshStarted = true;
  deviceLogf("AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
  deviceLogf("Station MAC: %s\n", WiFi.macAddress().c_str());
  deviceLogf("Mesh node ID: %u\n", mesh.getNodeId());
  timeSlicing.topologyChanged();
  return true;
}

void stopMeshForMeasurement() {
  meshStarted = false;
  mesh.stopForMeasurement();
  // AP off, station driver retained for ESP-NOW/sniffing. Avoid repeated
  // teardown/recreation implicated by the field log's STOP_STATE warning.
  if (!WiFi.mode(WIFI_STA)) deviceLog("[RADIO] Could not switch mesh radio to station mode");
}

void runTimeSlicing(bool armOnly) {
  if (armOnly) {
    timeSlicing.armConfiguredRound();
  } else {
    timeSlicing.update();
  }
}

bool userInputBusy() {
  return timeSlicing.busy() || timeSlicing.phase() == TimeSlicingController::Phase::FAILED;
}

void handleSerialCommand(const char *line, bool busyWhileReading) {
#if ROOT_FIRMWARE
  if (calibrationMode.localSession()) {
    calibrationMode.command(line);
    return;
  }
#endif
  if (busyWhileReading || userInputBusy()) {
    deviceLog("[ERROR] Round running: user input disabled until completion");
    return;
  }
#if ROOT_FIRMWARE
  const auto command = UserCommand::parse(line);
  if (command.kind == UserCommand::Kind::Invalid) {
    deviceLog("[ERROR] Use: conf | sniff <12-hex-MAC> [channel 1..13] | an opt <board 2..15> | an diag <board 2..15> [distance 1..5]");
    return;
  }
  if (command.kind == UserCommand::Kind::Calibrate) {
    calibrationMode.start(command.board);
    return;
  }
  if (command.kind == UserCommand::Kind::Diagnostic) {
    calibrationMode.start(command.board, true, command.distance);
    return;
  }
  if (!calibrationMode.networkSynced()) {
    deviceLog("[ERROR] Wait for all configured nodes to acknowledge the root's A/n");
    return;
  }
  if (!timeSlicing.networkReadyForConfiguration(macList.size())) {
    deviceLogf("[ERROR] Wait for stable mesh: %u/%u reachable peers\n",
        meshStarted ? static_cast<unsigned>(mesh.getNodeList().size()) : 0,
        static_cast<unsigned>(macList.size() - 1));
    return;
  }
  const bool started = command.kind == UserCommand::Kind::Conf
      ? runConfig() : timeSlicing.beginSniff(macList, command.target, command.channel);
  if (!started) deviceLog("[ERROR] Could not start round; check calibration and mesh");
#else
  deviceLog("[ERROR] Enter commands on root station 01 only");
#endif
}

// Bounded, nonblocking input. Busy input is rejected, never saved for later.
// Remember busy state even if a partial line finishes after the round ends.
void processSerialInput() {
  static UserCommand::LineReader reader;
  unsigned budget = 64;
  while (budget-- && Serial.available() > 0) {
    const auto result = reader.push(static_cast<char>(Serial.read()),
        !calibrationMode.localSession() && userInputBusy());
    using Result = UserCommand::LineReader::Result;
    if (result == Result::Busy) handleSerialCommand(reader.text(), true);
    else if (result == Result::Invalid)
      deviceLog("[ERROR] Invalid command characters or line longer than 63 characters");
    else if (result == Result::Line) handleSerialCommand(reader.text(), false);
  }
}

uint32_t lastMeshRecoveryMs = 0;
void recoverUnavailableMesh() {
  const auto phase = timeSlicing.phase();
  if (meshStarted || (phase != TimeSlicingController::Phase::IDLE &&
                      phase != TimeSlicingController::Phase::FAILED)) return;
  const uint32_t now = millis();
  if (now - lastMeshRecoveryMs < 5000) return;
  lastMeshRecoveryMs = now;
  deviceLog("[MESH] Retrying unavailable mesh");
  if (restartMesh()) timeSlicing.meshRecovered();
}

void setup() {
  Serial.begin(115200);
  beginModeIndicator();
  #if ROOT_FIRMWARE
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  String wsPath = String("/ws?role=device&token=") + DEVICE_TOKEN;
  webSocket.begin(BACKEND_HOST, BACKEND_PORT, wsPath.c_str());

  deviceLog("[WS] Connecting to backend...");
#endif
  calibrationMode.begin();
#if ROOT_FIRMWARE
  for (uint8_t board = 1; board <= TEST_BOARD_COUNT; ++board) {
    macList.push_back(MacAddress{0x44, 0x33, 0x22, 0x11, 0x00, board});
  }
#endif
  restartMesh();
  lastMeshRecoveryMs = millis();
#if ROOT_FIRMWARE
  deviceLog("[COMMAND] conf | sniff <12-hex-MAC> [channel] | an opt <board> | an diag <board> [distance 1..5] (newline, 115200 baud)");
#endif
}

void loop() {
#if ROOT_FIRMWARE
  webSocket.loop();
  flushDeviceLogQueue();

  if (uiSnapshotPending && webSocket.isConnected()) {
    uiSnapshotPending = false;

    for (uint8_t board = 1; board <= TEST_BOARD_COUNT; ++board) {
      if (uiNodeState[board].length()) {
        webSocket.sendTXT(uiNodeState[board]);
      }
    }
  }

  if (wsCommandPending) {
    wsCommandPending = false;

    uint32_t previousRoundId = timeSlicing.roundId();
    handleSerialCommand(wsCommandBuffer, false);

    if (timeSlicing.roundId() != previousRoundId) {
      uiConfInProgress = true;

      for (uint8_t board = 1; board <= TEST_BOARD_COUNT; ++board) {
        publishNodeState(board, board == 1 ? "root" : "configuring");
      }
    }
  }

  static int previousWifiStatus = -1;
  static String previousWifiIP = "";

  int currentWifiStatus = WiFi.status();
  String currentWifiIP = WiFi.localIP().toString();

  if (currentWifiStatus != previousWifiStatus ||
      currentWifiIP != previousWifiIP) {
    deviceLogf("[WIFI] status=%d IP=%s\n",
                  currentWifiStatus, currentWifiIP.c_str());

    previousWifiStatus = currentWifiStatus;
    previousWifiIP = currentWifiIP;
  }
#endif

  processSerialInput();
  recoverUnavailableMesh();

  if (meshStarted) {
    mesh.update();
  }

  // Must run on both the root and normal nodes.
  runTimeSlicing();

#if ROOT_FIRMWARE
  auto phase = timeSlicing.phase();

  if (uiConfInProgress &&
      (phase == TimeSlicingController::Phase::IDLE ||
       phase == TimeSlicingController::Phase::FAILED)) {
    for (uint8_t board = 2; board <= TEST_BOARD_COUNT; ++board) {
      String reportKey = String(board);
      bool receivedReport = roundReports[reportKey].is<JsonObject>();

      const char* status =
          phase == TimeSlicingController::Phase::FAILED ? "failed" :
          receivedReport ? "configured" : "missing";

      publishNodeState(board, status);
    }

    uiConfInProgress = false;
  }
#endif

  calibrationMode.update();

  auto radioActivity = timeSlicing.radioActivity();
  if (calibrationMode.active() && !calibrationMode.identifying() &&
      radioActivity == TimeSliceProtocol::RadioActivity::None) {
    radioActivity = TimeSliceProtocol::RadioActivity::Waiting;
  }

  updateModeIndicator(
      calibrationMode.identifying() ? String("scan") : currentMode,
      (!meshStarted &&
       timeSlicing.phase() != TimeSlicingController::Phase::MEASURING) ||
          timeSlicing.phase() == TimeSlicingController::Phase::FAILED,
      mesh.broadcastIndicatorActive(),
      radioActivity);

  delay(1);
}

