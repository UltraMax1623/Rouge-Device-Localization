//imports
#include <WiFi.h>
#include "esp_wifi.h"
#include "painlessMesh.h"




//Defines

#define MESH_PREFIX     "MyMeshNetwork"
#define MESH_PASSWORD   "somethingSneaky"
#define MESH_PORT       5555

//global const
uint8_t target_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
uint8_t self_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
uint8_t true_channel = 0;
bool channel_found = 0;
int packet_count =  0;
int RSSI = 0;
Scheduler userScheduler;
painlessMesh mesh;
//funtions


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

bool root_mssg_broadcast(char* mssg){
  // tartget_mac[6]
  // channel_found
  delay(1000);
  root_mode_switch();
}; //false if broadcast fails

bool external_mssg_parser(){
  // target_mac
  // configure
  root_mssg_broadcast("target_mac=XXXXXX,channel_found=0");
}; // retrun 1 if data is in expected format else return 1

void node_mode_switch();

bool node_mssg_parser(){
  // target_mac
  // channel_found
  node_mode_switch();
};

bool node_RSSI_send();

bool node_channel_send();

int ML_predict();

bool root_mssg_parser(){
  //RSSI
  //channel
  ML_predict();
}





//setup
void setup() {
  Serial.begin(11520);
  
  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  
  userScheduler.addTask(taskSendMessage);
  taskSendMessage.enable();
}

void loop() {

}
