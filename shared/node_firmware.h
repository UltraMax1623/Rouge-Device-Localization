#pragma once

//imports
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
// Path-loss calibration from the standalone localization sketch.
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

// Root command entry point; normal nodes receive configuration over mesh.
bool runConfig() {
  return timeSlicing.beginConfiguration(macList);
}

bool handleConfigMode(JsonDocument &doc, uint32_t from){
  // Other modes will be handled separately later.
  const char *mode = doc["Mode"] | "";

  if (strcmp(mode, "conf") != 0) {
    return false;
  }

  // Configuration must be marked root -> nodes.
  if (!doc["RootDirection"].is<int>() ||
      doc["RootDirection"].as<int>() != 1) {
    Serial.println("Configuration rejected: invalid direction");
    return false;
  }

  if (!doc["MacList"].is<JsonObject>()) {
    Serial.println("Configuration rejected: missing/invalid MacList");
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
      Serial.printf("Configuration rejected: invalid entry %s\n",
                    key.c_str());
      return false;
    }

    JsonArray bytes = receivedList[key].as<JsonArray>();

    if (bytes.size() != 6) {
      Serial.println("Configuration rejected: MAC needs six bytes");
      return false;
    }

    MacAddress mac{};

    for (size_t j = 0; j < mac.size(); ++j) {
      if (!bytes[j].is<unsigned int>() ||
          bytes[j].as<unsigned int>() > 255) {
        Serial.println("Configuration rejected: invalid MAC byte");
        return false;
      }

      mac[j] = static_cast<uint8_t>(bytes[j].as<unsigned int>());
    }

    for (const MacAddress &existing : newMacList) {
      if (existing == mac) {
        Serial.println("Configuration rejected: duplicate MAC");
        return false;
      }
    }

    newMacList.push_back(mac);
  }

  // The controller also verifies the root, round ID, and timing parameters.
  if (!timeSlicing.acceptConfiguration(doc, newMacList, from)) {
    Serial.println("Configuration rejected: root, round, or station list invalid");
    return false;
  }

  // Apply only after every entry has passed validation.
  macList = std::move(newMacList);
  runTimeSlicing(true);  // Arm only; radio changes happen later in loop().

  Serial.printf("Configuration stored: %u MACs\n",
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
    Serial.printf("JSON error: %s\n", error.c_str());
    return false;
  }

  const char *mode = doc["Mode"] | "";
  const char *action = doc["Action"] | "";

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

  Serial.println("Message rejected: missing or unknown Mode");
  return false;
}

void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Message from %u\n", from);
  node_mssg_parser(message, from);
}

void newConnectionCallback(uint32_t nodeId) {
  timeSlicing.topologyChanged();
  Serial.printf("Connected: %u\n", nodeId);
}

void changedConnectionCallback() {
  timeSlicing.topologyChanged();
  Serial.println("Mesh connections changed");
}

void nodeTimeAdjustedCallback(int32_t offset) {
  timeSlicing.timeAdjusted(offset);
  Serial.printf("Mesh time adjusted: %ld\n", static_cast<long>(offset));
}

bool configureSelfMacs() {
#if !ROOT_FIRMWARE
  if (esp_read_mac(AP_self_mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    Serial.println("Cannot read factory AP identity");
    return false;
  }
#endif
  if (memcmp(AP_self_mac, station_self_mac, 6) == 0 ||
      (AP_self_mac[0] & 1) || (station_self_mac[0] & 1)) {
    Serial.println("Invalid MACs: use distinct unicast addresses");
    return false;
  }

  // Core 3.x painlessMesh derives its ID using esp_read_mac(). Register the
  // custom addresses there as well as in the Wi-Fi driver.
#if ROOT_FIRMWARE
  if (esp_iface_mac_addr_set(AP_self_mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    Serial.println("Custom root AP registration failed");
    return false;
  }
#endif
  if (esp_iface_mac_addr_set(station_self_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    Serial.println("Custom interface MAC registration failed");
    return false;
  }

  // Initialize the Arduino Wi-Fi driver, then stop the interfaces before
  // changing their addresses. Restart without deinitializing the driver.
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_AP_STA)) {
    Serial.println("Wi-Fi initialization failed");
    return false;
  }

  esp_err_t error = esp_wifi_stop();
  if (error != ESP_OK) {
    Serial.printf("Wi-Fi stop failed: %s\n", esp_err_to_name(error));
    return false;
  }

#if ROOT_FIRMWARE
  error = esp_wifi_set_mac(WIFI_IF_AP, AP_self_mac);
  if (error != ESP_OK) {
    Serial.printf("AP MAC setup failed: %s\n", esp_err_to_name(error));
    return false;
  }

#endif

  error = esp_wifi_set_mac(WIFI_IF_STA, station_self_mac);
  if (error != ESP_OK) {
    Serial.printf("Station MAC setup failed: %s\n", esp_err_to_name(error));
    return false;
  }

  error = esp_wifi_start();
  if (error != ESP_OK) {
    Serial.printf("Wi-Fi start failed: %s\n", esp_err_to_name(error));
    return false;
  }
  return true;
}

bool restartMesh() {
  meshStarted = false;
  if (!configureSelfMacs()) {
    Serial.println("Mesh not started: check custom MAC configuration");
    return false;
  }

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT,
            WIFI_AP_STA, TimeSliceProtocol::CHANNEL);
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
    Serial.println("Custom MAC verification failed; stopping mesh");
    mesh.stopForMeasurement();
    return false;
  }

  meshStarted = true;
  Serial.printf("AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("Station MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Mesh node ID: %u\n", mesh.getNodeId());
  timeSlicing.topologyChanged();
  return true;
}

void stopMeshForMeasurement() {
  meshStarted = false;
  mesh.stopForMeasurement();
  WiFi.mode(WIFI_OFF);
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
  if (busyWhileReading || userInputBusy()) {
    Serial.println("[ERROR] Round running: user input disabled until completion");
    return;
  }
#if ROOT_FIRMWARE
  const auto command = UserCommand::parse(line);
  if (command.kind == UserCommand::Kind::Invalid) {
    Serial.println("[ERROR] Use: conf | sniff <12-hex-MAC> [channel 1..13]");
    return;
  }
  if (!timeSlicing.networkReadyForConfiguration(macList.size())) {
    Serial.printf("[ERROR] Wait for stable mesh: %u/%u reachable peers\n",
        meshStarted ? static_cast<unsigned>(mesh.getNodeList().size()) : 0,
        static_cast<unsigned>(macList.size() - 1));
    return;
  }
  const bool started = command.kind == UserCommand::Kind::Conf
      ? runConfig() : timeSlicing.beginSniff(macList, command.target, command.channel);
  if (!started) Serial.println("[ERROR] Could not start round; check calibration and mesh");
#else
  Serial.println("[ERROR] Enter commands on root station 01 only");
#endif
}

// Bounded, nonblocking input. Busy input is rejected, never saved for later.
// Remember busy state even if a partial line finishes after the round ends.
void processSerialInput() {
  static UserCommand::LineReader reader;
  unsigned budget = 64;
  while (budget-- && Serial.available() > 0) {
    const auto result = reader.push(static_cast<char>(Serial.read()), userInputBusy());
    using Result = UserCommand::LineReader::Result;
    if (result == Result::Busy) handleSerialCommand(reader.text(), true);
    else if (result == Result::Invalid)
      Serial.println("[ERROR] Invalid command characters or line longer than 63 characters");
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
  Serial.println("[MESH] Retrying unavailable mesh");
  if (restartMesh()) timeSlicing.meshRecovered();
}

void setup() {
  Serial.begin(115200);
  beginModeIndicator();
#if ROOT_FIRMWARE
  for (uint8_t board = 1; board <= TEST_BOARD_COUNT; ++board) {
    macList.push_back(MacAddress{0x44, 0x33, 0x22, 0x11, 0x00, board});
  }
#endif
  restartMesh();
  lastMeshRecoveryMs = millis();
#if ROOT_FIRMWARE
  Serial.println("[COMMAND] Enter conf or sniff <12-hex-MAC> (newline, 115200 baud)");
#endif
}

void loop() {
  processSerialInput();
  recoverUnavailableMesh();
  if (meshStarted) {
    mesh.update();
  }
  runTimeSlicing();
  updateModeIndicator(currentMode,
      (!meshStarted && timeSlicing.phase() != TimeSlicingController::Phase::MEASURING)
          || timeSlicing.phase() == TimeSlicingController::Phase::FAILED,
      mesh.broadcastIndicatorActive());
  delay(1);
}
