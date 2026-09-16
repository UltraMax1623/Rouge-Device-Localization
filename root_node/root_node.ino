//imports
#include <Wifi.h>
#include "esp_wifi.h"


//Defines



//global const
uint8_t target_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
uint8_t self_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
unit8_t true_channel = 0;
bool channel_found = 0;
int packet_count =  0;
int RSSI = 0;

//funtions

void root_mode_switch(){
  sniff_mode();
};

bool root_mssg_broadcast(){
  // tartget_mac[6]
  // channel_found
  delay(1000);
  root_mode_switch()
}; //false if broadcast fails

bool external_mssg_parser(){
  // target_mac
  // configure
  root_mssg_broadcast("target_mac=XXXXXX,channek_found=0");
}; // retrun 1 if data is in expected format else return 1

void node_mode_switch();

bool node_mssg_parser(){
  // target_mac
  // channel_found
  node_mode_switch();
};

bool node_RSSI_send();

bool node_channel_send();

bool root_sniff_mode();
bool node_sniff_mode();

int ML_predict();

bool root_mssg_parser(){
  //RSSI
  //channel
  ML_predict();
}





//setup
void setup() {
  Serial.begin(11520);
}

void loop() {

}
