#include <WiFi.h>
#include "esp_wifi.h"

// ===============================
// Target MAC address
// ===============================
uint8_t targetMAC[6] = {
  0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33
};

// ===============================
// Current Wi-Fi channel
// ===============================
volatile int currentChannel = 1;

// ===============================
// Compare two MAC addresses
// ===============================
bool sameMAC(uint8_t *mac1, uint8_t *mac2) {

  for (int i = 0; i < 6; i++) {

    if (mac1[i] != mac2[i]) {
      return false;
    }
  }

  return true;
}

// ===============================
// Print MAC address
// ===============================
void printMAC(uint8_t *mac) {

  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2],
    mac[3], mac[4], mac[5]);
}

// ===============================
// Sniffer callback
// ===============================
void sniffer(void *buf, wifi_promiscuous_pkt_type_t type) {

  // Only process management and data frames
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) {
    return;
  }

  wifi_promiscuous_pkt_t *packet =
    (wifi_promiscuous_pkt_t *)buf;

  // Raw 802.11 frame
  uint8_t *frame = packet->payload;

  // ===============================
  // Get Frame Control field
  // ===============================

  uint16_t frameControl =
    frame[0] | (frame[1] << 8);

  // Get frame type
  // 0 = Management
  // 1 = Control
  // 2 = Data
  uint8_t frameType =
    (frameControl >> 2) & 0x03;

  // We only want management/data
  if (frameType != 0 && frameType != 2) {
    return;
  }

  // ===============================
  // Find Transmitter MAC (TA)
  // ===============================

  uint8_t *transmitterMAC;

  // -------------------------------
  // Management frame
  // -------------------------------
  if (frameType == 0) {

    // Address 2 = Transmitter Address
    transmitterMAC = frame + 10;
  }

  // -------------------------------
  // Data frame
  // -------------------------------
  else {

    bool toDS =
      frameControl & (1 << 8);

    bool fromDS =
      frameControl & (1 << 9);

    if (!toDS && !fromDS) {

      // Address 2 = Transmitter
      transmitterMAC = frame + 10;
    }

    else if (toDS && !fromDS) {

      // Address 2 = Transmitter
      transmitterMAC = frame + 10;
    }

    else if (!toDS && fromDS) {

      // Address 3 = Transmitter
      transmitterMAC = frame + 16;
    }

    else {

      // WDS frame
      // Address 4 = Transmitter
      transmitterMAC = frame + 24;
    }
  }

  // ===============================
  // Check if TARGET transmitted it
  // ===============================

  if (!sameMAC(transmitterMAC, targetMAC)) {
    return;
  }

  // ===============================
  // Target packet found
  // ===============================

  int rssi = packet->rx_ctrl.rssi;

  Serial.print("TARGET TRANSMITTER FOUND");

  Serial.print(" | MAC: ");
  printMAC(transmitterMAC);

  Serial.print(" | RSSI: ");
  Serial.print(rssi);
  Serial.print(" dBm");

  Serial.print(" | Channel: ");
  Serial.println(currentChannel);
}

// ===============================
// Setup
// ===============================
void setup() {

  Serial.begin(115200);

  // Enable Wi-Fi station mode
  WiFi.mode(WIFI_STA);

  // Do not connect to an access point
  WiFi.disconnect();

  // Set callback
  esp_wifi_set_promiscuous_rx_cb(sniffer);

  // Enable promiscuous/sniffer mode
  esp_wifi_set_promiscuous(true);
}

// ===============================
// Main loop
// ===============================
void loop() {

  // Hop through channels 1-13

  for (currentChannel = 1;
       currentChannel <= 13;
       currentChannel++) {

    // Change Wi-Fi channel
    esp_wifi_set_channel(
      currentChannel,
      WIFI_SECOND_CHAN_NONE);

    // Listen on this channel
    delay(100);
  }
}