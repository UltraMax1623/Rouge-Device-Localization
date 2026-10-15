#include <WiFi.h>
#include <cmath>
#include "esp_wifi.h"

uint8_t selfMAC[6] = { 0x44, 0x33, 0x22, 0x11, 0x00, 0x02 };
uint8_t targetMAC[6] = { 0x44, 0x33, 0x22, 0x11, 0x00, 0x03 };
volatile unsigned long start1 = millis();
volatile int currentChannel = 1;
volatile int packetcounter = 0;
volatile double totalrssi = 0;
volatile bool targetPacketFlag = false;
volatile double latestRssi = 0;
uint8_t capturedMac[6];

// Path loss model parameters
volatile double A = -60.0;
volatile double n = 2.0; //1 - 4
bool sameMAC(const uint8_t *mac1, const uint8_t *mac2) {
  for (int i = 0; i < 6; i++) {
    if (mac1[i] != mac2[i]) return false;
  }
  return true;
}
void printMAC(uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
double rssiToDistance(double rssi,double A,double n) {
  return pow(10.0, (A - rssi) / (10.0 * n));
}
void rssilogger(double rssi){
  totalrssi += rssi;
  packetcounter++;;
}
void IRAM_ATTR sniffer(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *frame = packet->payload;

    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t frameType = (frameControl >> 2) & 0x03;

    if (frameType != 0 && frameType != 2) return;

    uint8_t *transmitterMAC;

    if (frameType == 0) {
        transmitterMAC = frame + 10;
    } else {
        bool toDS = frameControl & (1 << 8);
        bool fromDS = frameControl & (1 << 9);

        if (!toDS && !fromDS) transmitterMAC = frame + 10;
        else if (toDS && !fromDS) transmitterMAC = frame + 10;
        else if (!toDS && fromDS) transmitterMAC = frame + 16;
        else transmitterMAC = frame + 24;
    }

    if (!sameMAC(transmitterMAC, targetMAC)) {
        return;
    }

    // Safely capture data and let loop() do the heavy lifting
    latestRssi = packet->rx_ctrl.rssi;
    memcpy(capturedMac, transmitterMAC, 6);
    targetPacketFlag = true;
}
void setup() {
  Serial.begin(115200);

  // Essential for ESP32-S3 native USB CDC output
  unsigned long start = millis();
  while (!Serial && (millis() - start < 10000));

  Serial.println("\n--- ESP32-S3 Wi-Fi Sniffer Starting ---");

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, &selfMAC[0]);
  if (err == ESP_OK) {
    Serial.println("Successfully changed Station MAC address!");
  } else {
    Serial.println("Failed to change MAC address");
  }
  Serial.print("New MAC: ");
  Serial.println(WiFi.macAddress());
  WiFi.disconnect();
  esp_wifi_set_promiscuous_rx_cb(sniffer);
  esp_wifi_set_promiscuous(true);
}
void loop() {
    if (targetPacketFlag) {
        targetPacketFlag = false; // Reset flag

        if (millis() - start1 < 7000) {
            rssilogger(latestRssi);
            totalrssi += latestRssi;
            packetcounter++;
        } else {
            if (packetcounter > 0) {
                double avgRssi = totalrssi / packetcounter;
                Serial.print("MAC: ");
                printMAC(capturedMac);
                Serial.printf(" | Avg RSSI: %.2f dBm\n",avgRssi);
                double distance = rssiToDistance(avgRssi, A, n);
                Serial.printf("Dist: %.4f m | A: %.4f | n: %.4f\n",distance, A, n);
                Serial.print("\n\n\n\n");
            }
            start1 = millis();
            packetcounter = 0;
            totalrssi = 0;
        }
    }
    if (Serial.available() > 0){
    String inputString = Serial.readStringUntil('\n');
    inputString.trim();
    char charBuffer[50];
    inputString.toCharArray(charBuffer, sizeof(charBuffer));
    int itemsRead = sscanf(charBuffer, "%lf %lf", &A, &n);
    if (itemsRead == 2) {
      packetcounter = 0;
      totalrssi = 0;
      Serial.print("Successfully parsed! Value 1: ");
      Serial.print(A, 4); // Print with 4 decimal places
      Serial.print(" | Value 2: ");
      Serial.println(n, 4);
    } else {
      Serial.println("Error: Please format as 'number1 number2'");
    }
  }
    // Feed the task watchdog on CPU 1
    delay(2); 
}