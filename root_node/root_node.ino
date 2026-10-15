//imports
#include <WiFi.h>
#include "esp_wifi.h"
#include "painlessMesh.h"
#include <ArduinoJson.h>
#include <array>
#include <vector>
#include <utility>


//Defines

#define MESH_PREFIX     "MeshNetwork"
#define MESH_PASSWORD   "pavansqaured"
#define MESH_PORT       5555

//global const
uint8_t target_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
// Give every board a unique pair; these defaults are for one board only.
uint8_t AP_self_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFE };
uint8_t station_self_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
bool meshStarted = false;
uint8_t true_channel = 0;
bool channel_found = 0;
int packet_count =  0;
int RSSI = 0;
Scheduler userScheduler;
painlessMesh mesh;
JsonDocument meshmess;
using MacAddress = std::array<uint8_t, 6>;
std::vector<MacAddress> macList;
String currentMode = "mesh";

//funtions
bool node_mssg_parser(const String &message);
void receivedCallback(uint32_t from, String &message);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
void nodeTimeAdjustedCallback(int32_t offset);
bool configureSelfMacs();


//input node mac to PC
//PC creates MAC table
//add mac function
//remove mac function
//pc sends MAC table to root
//root populates all nodes with mac table


bool root_sniff_mode();
bool node_sniff_mode();


void root_mode_switch(){
  root_sniff_mode();
};

// Call on the root only, after its macList has been populated.
bool runConfig() {
  JsonDocument doc;

  doc["Mode"] = "conf";
  doc["RootDirection"] = 1;

  JsonObject jsonMacList = doc["MacList"].to<JsonObject>();

  for (size_t i = 0; i < macList.size(); ++i) {
    for (size_t previous = 0; previous < i; ++previous) {
      if (macList[i] == macList[previous]) {
        Serial.println("Configuration failed: duplicate MAC");
        return false;
      }
    }

    String key = String(static_cast<unsigned long>(i + 1));

    JsonArray bytes = jsonMacList[key].to<JsonArray>();

    for (uint8_t byte : macList[i]) {
      bytes.add(byte);
    }
  }

  if (doc.overflowed()) {
    Serial.println("Configuration failed: JSON allocation error");
    return false;
  }

  String message;
  size_t expectedLength = measureJson(doc);

  if (serializeJson(doc, message) != expectedLength) {
    Serial.println("Configuration failed: serialization incomplete");
    return false;
  }

  bool queued = mesh.sendBroadcast(message);
  if (queued) {
    currentMode = "conf";
  }

  Serial.printf(
    "Configuration: %u MACs | broadcast %s\n",
    static_cast<unsigned>(macList.size()),
    queued ? "queued" : "failed"
  );

  return queued;
}

bool root_mssg_broadcast(const char* mssg){
  // tartget_mac[6]
  // channel_found
  delay(1000);
  root_mode_switch();
  return false;
}; //false if broadcast fails

bool external_mssg_parser(){
  // target_mac
  // configure
  root_mssg_broadcast("target_mac=XXXXXX,channel_found=0");
  return false;
}; // retrun 1 if data is in expected format else return 1

void node_mode_switch();

bool handleConfigMode(JsonDocument &doc){
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

  // Apply only after every entry has passed validation.
  macList = std::move(newMacList);
  currentMode = "conf";

  Serial.printf("Configuration stored: %u MACs\n",
                static_cast<unsigned>(macList.size()));

  return true;
}
bool handleMeshMode(JsonDocument &doc) {
  // TODO: Implement mesh mode later.
  return false;
}

bool handleSniffMode(JsonDocument &doc) {
  // TODO: Implement sniff mode later.
  return false;
}

bool node_mssg_parser(const String &message) {
  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.printf("JSON error: %s\n", error.c_str());
    return false;
  }

  const char *mode = doc["Mode"] | "";

  if (strcmp(mode, "conf") == 0) {
    return handleConfigMode(doc);
  }

  if (strcmp(mode, "mesh") == 0) {
    return handleMeshMode(doc);
  }

  if (strcmp(mode, "sniff") == 0) {
    return handleSniffMode(doc);
  }

  Serial.println("Message rejected: missing or unknown Mode");
  return false;
}

bool node_RSSI_send();

bool node_channel_send();

int ML_predict();

bool root_mssg_parser(){
  //RSSI
  //channel
  ML_predict();
  return false;
}





void receivedCallback(uint32_t from, String &message) {
  Serial.printf("Message from %u\n", from);
  node_mssg_parser(message);
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("Connected: %u\n", nodeId);
}

void changedConnectionCallback() {
  Serial.println("Mesh connections changed");
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("Mesh time adjusted: %ld\n", static_cast<long>(offset));
}

bool configureSelfMacs() {
  if (memcmp(AP_self_mac, station_self_mac, 6) == 0 ||
      (AP_self_mac[0] & 1) || (station_self_mac[0] & 1)) {
    Serial.println("Invalid MACs: use distinct unicast addresses");
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

  error = esp_wifi_set_mac(WIFI_IF_AP, AP_self_mac);
  if (error != ESP_OK) {
    Serial.printf("AP MAC setup failed: %s\n", esp_err_to_name(error));
    return false;
  }

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

//setup
void setup() {
  Serial.begin(115200);

  if (!configureSelfMacs()) {
    Serial.println("Mesh not started: check custom MAC configuration");
    return;
  }
  
  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  uint8_t actualAP[6];
  uint8_t actualStation[6];
  if (esp_wifi_get_mac(WIFI_IF_AP, actualAP) != ESP_OK ||
      esp_wifi_get_mac(WIFI_IF_STA, actualStation) != ESP_OK ||
      memcmp(actualAP, AP_self_mac, 6) != 0 ||
      memcmp(actualStation, station_self_mac, 6) != 0) {
    Serial.println("Custom MAC verification failed; stopping mesh");
    mesh.stop();
    return;
  }

  meshStarted = true;
  Serial.printf("AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("Station MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Mesh node ID: %u\n", mesh.getNodeId());
  
}

void loop() {
  if (meshStarted) {
    mesh.update();
  }
}
