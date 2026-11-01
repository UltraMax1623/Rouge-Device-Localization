#pragma once
#include "target_position.h"

// Root-only command handlers. Normal nodes continue using the existing mesh protocol.
inline bool webCommandSupported(const String &name) {
  return name=="conf" || name=="sniff" || name=="status" || name=="calibrate" || name=="diagnose";
}

void networkSnapshot(JsonDocument &doc) {
  doc["kind"]="network"; doc["rootMac"]=configuredMac(1);
  doc["serverIP"]=backendHost; doc["serverPort"]=BACKEND_PORT;
  doc["ssid"]=WiFi.SSID();doc["ip"]=WiFi.localIP().toString();
  doc["gateway"]=WiFi.gatewayIP().toString();doc["rssi"]=WiFi.RSSI();
  doc["meshChannel"]=meshChannel;doc["routerChannel"]=channelMigration.routerChannel();
  doc["mode"]=currentMode;doc["activeCommand"]=webCommandRunning ? activeCommandName : "idle";
  doc["A"]=A;doc["n"]=n;doc["modelSynced"]=calibrationMode.networkSynced();
  doc["uptimeSeconds"]=millis()/1000;doc["freeHeap"]=ESP.getFreeHeap();
  doc["migration"]=channelMigration.debugPhase();doc["layoutRoundId"]=lastCoordinateRound;
  JsonArray nodes=doc["nodes"].to<JsonArray>();
  for(unsigned board=1;board<=TEST_BOARD_COUNT;++board) {
    JsonObject node=nodes.add<JsonObject>();node["board"]=board;node["mac"]=configuredMac(board);
    node["online"]=calibrationMode.nodeOnline(board);
    if(lastNodeCoordinates.points[board-1].valid) {
      node["x"]=lastNodeCoordinates.points[board-1].x;
      node["y"]=lastNodeCoordinates.points[board-1].y;
    }
  }
}

void publishNetworkStatus() {
  static uint32_t last=0;
  if(last && millis()-last<5000) return;
  last=millis();
  JsonDocument doc;networkSnapshot(doc);doc["type"]="network_status";
  String payload;serializeJson(doc,payload);webSocket.sendTXT(payload);
}

void endWebCommand(const char *status) {
  JsonDocument end;commandEnvelope(end,activeCommandId,"end");end["status"]=status;
  queueCommandEvent(end);webCommandRunning=false;activeCommandId="";
}

bool beginWebLifecycle(const String &id) {
  JsonDocument start;commandEnvelope(start,id,"start");
  start["status"]="accepted";start["boards"]=TEST_BOARD_COUNT;
  queueCommandEvent(start);
  if(!webSocket.isConnected() || !commandEvents.send(webSocket,true)) {
    rejectCommand(id,"START_ACK_FAILED","Could not send the start acknowledgement.");return false;
  }
  activeCommandId=id;webCommandRunning=true;webCancelled=false;webSnapshot="";
  return true;
}

void dispatchWebCommand(const String &id,const String &name,JsonDocument &params) {
  if(webCommandRunning) {
    // Backend prevents this; never overwrite the running command's identity.
    return;
  }
  activeCommandName=name;
  if(!webCommandSupported(name)) { rejectCommand(id,"UNKNOWN_COMMAND","Unsupported command.");return; }
  if(commandEvents.available() || !commandEvents.room(TEST_BOARD_COUNT+7)) {
    rejectCommand(id,"RESULTS_PENDING","Waiting for delivery acknowledgements.");return;
  }
  webCancelled=false;webSnapshot="";
  if(timeSlicing.busy() || calibrationMode.active() || channelMigration.busy() ||
      timeSlicing.phase()==TimeSlicingController::Phase::FAILED) {
    rejectCommand(id,"BUSY","The radio is busy or recovering.");return;
  }
  if(name=="status") {
    if(!beginWebLifecycle(id))return;
    JsonDocument data;networkSnapshot(data);commandEnvelope(data,id,"data");queueCommandEvent(data);
    endWebCommand("success");return;
  }
  if(!calibrationMode.networkSynced() || !timeSlicing.networkReadyForConfiguration(macList.size())) {
    rejectCommand(id,"MESH_NOT_READY","Wait for the configured nodes and A/n acknowledgements.");return;
  }
  if(name=="conf") { startWebConf(id);return; }
  if(name=="sniff") {
    const String target=params["mac"] | "";
    const unsigned channel=params["channel"] | 0u;
    const String text=String("sniff ")+target+(channel ? String(" ")+channel : "");
    const auto command=UserCommand::parse(text.c_str());
    if(command.kind!=UserCommand::Kind::Sniff || channel>13) {
      rejectCommand(id,"INVALID_ARGUMENT","Use a 12-digit hexadecimal MAC and channel 0..13 (0 means automatic).");return;
    }
    for(const auto &mac:macList) if(mac==command.target) {
      rejectCommand(id,"INVALID_TARGET","Choose an external Wi-Fi device, not a configured mesh station.");return;
    }
    if(!beginWebLifecycle(id))return;
    webSniffTarget=command.target;
    if(!timeSlicing.beginSniff(macList,command.target,command.channel)) {
      rejectCommand(id,"START_FAILED","Could not start sniffing.");webCommandRunning=false;activeCommandId="";
    }
    return;
  }
  unsigned board=params["board"] | 0u, distance=params["distance"] | 1u;
  if(board<2 || board>TEST_BOARD_COUNT || distance<1 || distance>5) {
    rejectCommand(id,"INVALID_ARGUMENT","Select a configured normal board and distance 1..5 m.");return;
  }
  if(!beginWebLifecycle(id))return;
  if(!calibrationMode.start(board,name=="diagnose",distance)) {
    rejectCommand(id,"START_FAILED","Could not start the selected calibration/diagnostic session.");
    webCommandRunning=false;activeCommandId="";
  }
}

void processWebControl() {
  if(!pendingControlId.length() || timeSlicing.timingCritical())return;
  static bool lastAccepted=false;
  static String lastMessage;
  const String id=pendingControlId, action=pendingControlAction;
  pendingControlId="";pendingControlAction="";
  if(id!=lastControlId) {
    lastControlId=id;lastAccepted=false;
    if(webCommandRunning) {
      if(activeCommandName=="calibrate" || activeCommandName=="diagnose")
        lastAccepted=calibrationMode.webControl(action);
      else if(action=="cancel") lastAccepted=timeSlicing.requestCancellation();
    }
    if(lastAccepted && action=="cancel")webCancelled=true;
    lastMessage=lastAccepted ? "Action accepted." : "Action is unavailable in the current command state.";
  }
  JsonDocument ack;ack["type"]="control_ack";ack["controlId"]=id;
  ack["commandId"]=activeCommandId;ack["accepted"]=lastAccepted;ack["message"]=lastMessage;
  String payload;serializeJson(ack,payload);webSocket.sendTXT(payload);
}

void finishWebSniff() {
  char target[18];snprintf(target,sizeof(target),"%02X:%02X:%02X:%02X:%02X:%02X",
      webSniffTarget[0],webSniffTarget[1],webSniffTarget[2],webSniffTarget[3],webSniffTarget[4],webSniffTarget[5]);
  double ranges[Coordinates::MAX_NODES]={};unsigned validNodes=0,completeNodes=0;
  for(unsigned board=1;board<=TEST_BOARD_COUNT;++board) {
    String mac=configuredMac(board);const JsonVariantConst value=sniffAverageDistances[mac][target];
    unsigned count=sniffAverageCounts[mac] | 0u;
    const bool valid=count && value.is<double>() && std::isfinite(value.as<double>()) && value.as<double>()>0;
    if(valid) {ranges[board-1]=value.as<double>();++validNodes;}
    if(valid && count==TimeSliceProtocol::SNIFF_CYCLES)++completeNodes;
    JsonDocument data;commandEnvelope(data,activeCommandId,"data");
    data["kind"]="sniff";data["reporterMac"]=mac;data["targetMac"]=target;
    data["status"]=valid ? count==TimeSliceProtocol::SNIFF_CYCLES ? "ok" : "partial" : "unavailable";
    data["validCycles"]=count;data["cycles"]=TimeSliceProtocol::SNIFF_CYCLES;
    data["channel"]=timeSlicing.discoveredChannel;
    if(lastNodeCoordinates.points[board-1].valid) {
      data["position"]["x"]=lastNodeCoordinates.points[board-1].x;
      data["position"]["y"]=lastNodeCoordinates.points[board-1].y;
    }
    JsonObject reading=data["distances"].to<JsonArray>().add<JsonObject>();
    reading["targetMac"]=target;reading["status"]=valid ? "ok" : "unavailable";
    if(valid) reading["meters"]=ranges[board-1];else reading["meters"]=nullptr;
    queueCommandEvent(data);
  }
  const auto targetPosition=Coordinates::locateTarget(lastNodeCoordinates,ranges,TEST_BOARD_COUNT);
  JsonDocument data;commandEnvelope(data,activeCommandId,"data");data["kind"]="target";
  data["targetMac"]=target;data["layoutRoundId"]=lastCoordinateRound;
  data["anchors"]=targetPosition.anchors;data["ambiguous"]=targetPosition.ambiguous;
  if(targetPosition.point.valid) {
    data["position"]["x"]=targetPosition.point.x;data["position"]["y"]=targetPosition.point.y;
    data["position"]["status"]=targetPosition.ambiguous ? "assumed" : "estimated";
    data["position"]["fitRmseMeters"]=targetPosition.rmse;
  } else { data["position"]["x"]=nullptr;data["position"]["y"]=nullptr;data["position"]["status"]="unavailable"; }
  data["message"]=!lastNodeCoordinates.valid ? "Run Configure to establish anchor positions before locating a target." :
      targetPosition.ambiguous ? "Positive-quadrant position selected by convention; the ranges do not determine a unique position." : "Target fitted to the last configuration layout.";
  queueCommandEvent(data);
  if(!validNodes && !webCancelled) {
    JsonDocument error;commandEnvelope(error,activeCommandId,"error");error["code"]="TARGET_NOT_HEARD";
    error["message"]="No valid target averages. Check the Wi-Fi MAC, 2.4 GHz traffic, activity, and range.";queueCommandEvent(error);
  }
  endWebCommand(webCancelled ? "cancelled" : completeNodes==TEST_BOARD_COUNT ? "success" : validNodes ? "partial" : "failed");
}

void updateWebCommand() {
  processWebControl();
  if(!webCommandRunning)return;
  const auto phase=timeSlicing.phase();
  if(activeCommandName=="conf" || activeCommandName=="sniff") {
    if(timeSlicing.busy()) {
      if(activeCommandName=="sniff" && !timeSlicing.timingCritical() && commandEvents.room(TEST_BOARD_COUNT+5)) {
        String key=String(timeSlicing.captureCycle())+":"+timeSlicing.targetChannel();
        if(key!=webSnapshot) {
          webSnapshot=key;
          JsonDocument event;commandEnvelope(event,activeCommandId,"log");
          event["message"]=timeSlicing.captureCycle() ?
              String("Capture cycle ")+timeSlicing.captureCycle()+"/"+TimeSliceProtocol::SNIFF_CYCLES+
              " on channel "+timeSlicing.targetChannel() : String("Discovering the target Wi-Fi channel.");
          queueCommandEvent(event);
        }
      }
      return;
    }
    if(activeCommandName=="conf") { finishWebConf(phase);webCommandRunning=false; }
    else finishWebSniff();
    return;
  }
  if(activeCommandName=="calibrate" || activeCommandName=="diagnose") {
    const bool finished=!calibrationMode.localSession();
    if(!finished && (!calibrationMode.backendSafe() || !backendServiceAllowed() || !commandEvents.room(5)))return;
    JsonDocument snapshot;calibrationMode.snapshot(snapshot,activeCommandName=="diagnose");
    String serialized;serializeJson(snapshot,serialized);
    if(serialized!=webSnapshot || finished) {
      webSnapshot=serialized;commandEnvelope(snapshot,activeCommandId,"data");queueCommandEvent(snapshot);
    }
    if(finished) {
      if(activeCommandName=="calibrate" && strcmp(calibrationMode.outcome(),"success")==0) {
        lastNodeCoordinates={};lastCoordinateRound=0;
      }
      endWebCommand(calibrationMode.outcome());
    }
  }
}
