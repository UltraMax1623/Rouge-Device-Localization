#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

// Your custom unicast MAC address
uint8_t customMac[] = {0x44, 0x33, 0x22, 0x11, 0x00, 0x03};

// Broadcast address so any listening sniffer catches the frame
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void setup() {
  Serial.begin(115200);

  // 1. Initialize Wi-Fi in Station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // 2. Override the default MAC address with your custom one
  esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, &customMac[0]);
  if (err != ESP_OK) {
    Serial.println("Failed to set custom MAC address!");
  }

  // 3. Lock transmitter to a fixed Wi-Fi channel (e.g., Channel 1)
  // Both the transmitter and the sniffer MUST be on the exact same channel.
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // 4. Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // 5. Register the broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 1;  
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
    return;
  }
  
  Serial.print("Transmitter active with MAC: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  // Payload data to transmit
  uint8_t payload[] = "MESH_DISCOVER_PACKET";
  
  // Send broadcast packet via ESP-NOW
  esp_err_t result = esp_now_send(broadcastAddress, payload, sizeof(payload));
  
  if (result == ESP_OK) {
    Serial.println("Beacon packet broadcasted successfully.");
  } else {
    Serial.println("Transmission failed.");
  }
  
  delay(500); // Send twice a second
}