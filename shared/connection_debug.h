#pragma once

// Set to 0 after collecting a connection trace. Never prints credentials.
#ifndef CONNECTION_DEBUG
#define CONNECTION_DEBUG 1
#endif

bool connectionDebugAllowed();
inline void connectionTrace(const char *line) {
#if CONNECTION_DEBUG
  if (connectionDebugAllowed()) Serial.printf("[%lu ms] %s\n", millis(), line);
#endif
}

// Set to 0 after collecting the serial traces on all boards.
#ifndef MESH_REJOIN_DEBUG
#define MESH_REJOIN_DEBUG FIRMWARE_DEBUG
#endif

#if MESH_REJOIN_DEBUG
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace MeshRejoinTrace {
struct Event {
  WiFiEvent_t type;
  unsigned long at;
  uint8_t mac[6];
  unsigned value;
  uint32_t ip;
};

inline QueueHandle_t queue = nullptr;

// Wi-Fi callbacks run on another task. Copy only the event data and log from loop().
inline void capture(WiFiEvent_t type, WiFiEventInfo_t info) {
  Event event{type, millis(), {}, 0, 0};
  switch (type) {
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      event.value = info.wifi_scan_done.status;
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      memcpy(event.mac, info.wifi_sta_connected.bssid, sizeof(event.mac));
      event.value = info.wifi_sta_connected.channel;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      memcpy(event.mac, info.wifi_sta_disconnected.bssid, sizeof(event.mac));
      event.value = info.wifi_sta_disconnected.reason;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      event.ip = info.got_ip.ip_info.ip.addr;
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      memcpy(event.mac, info.wifi_ap_staconnected.mac, sizeof(event.mac));
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      memcpy(event.mac, info.wifi_ap_stadisconnected.mac, sizeof(event.mac));
      break;
    default:
      return;
  }
  xQueueSend(queue, &event, 0);
}

inline void begin() {
  queue = xQueueCreate(24, sizeof(Event));
  if (queue) WiFi.onEvent(capture);
  else FW_LOG("[REJOIN] Could not allocate Wi-Fi event queue");
}

inline void drain(const uint8_t *rootBssid, bool meshStarted) {
  Event event;
  if (!queue || xQueueReceive(queue, &event, 0) != pdTRUE) return;
  if (event.type == ARDUINO_EVENT_WIFI_SCAN_DONE) {
    // Inspect Arduino's completed scan cache before the next scheduler pass.
    // A stale event may refer to a cache that has already been replaced.
    const bool scanAvailable = meshStarted && event.value == 0 && millis() - event.at < 1000;
    const int count = scanAvailable ? WiFi.scanComplete() : -1;
    int rootIndex = -1;
    for (int i = 0; i < count && i <= UINT8_MAX; ++i) {
      const uint8_t *bssid = WiFi.BSSID(i);
      if (bssid && memcmp(bssid, rootBssid, sizeof(event.mac)) == 0) {
        rootIndex = i;
        break;
      }
    }
    const char *rootStatus = rootIndex >= 0 ? "seen" : (count >= 0 ? "absent" : "unknown");
    FW_LOG("[REJOIN] scan done at=%lu status=%u results=%d radio=%d root=%s channel=%d rssi=%d",
           event.at, event.value, count, WiFi.channel(),
           rootStatus,
           rootIndex >= 0 ? WiFi.channel(rootIndex) : 0, rootIndex >= 0 ? WiFi.RSSI(rootIndex) : 0);
  } else if (event.type == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    FW_LOG("[REJOIN] STA got IP at=%lu ip=%s radio=%d",
           event.at, IPAddress(event.ip).toString().c_str(), WiFi.channel());
  } else {
    const char *name = "AP client left";
    const char *detail = "";
    if (event.type == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
      name = "STA associated";
      detail = " channel=";
    } else if (event.type == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      name = "STA disconnected";
      detail = " reason=";
    } else if (event.type == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
      name = "AP client joined";
    }
    char suffix[20] = {};
    if (*detail) snprintf(suffix, sizeof(suffix), "%s%u", detail, event.value);
    FW_LOG("[REJOIN] %s at=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X%s",
           name, event.at, unsigned(event.mac[0]), unsigned(event.mac[1]), unsigned(event.mac[2]),
           unsigned(event.mac[3]), unsigned(event.mac[4]), unsigned(event.mac[5]), suffix);
  }
}
}
#endif
