#pragma once
#include "target_position.h"
#if CONFIG_IDF_TARGET_ESP32S3 && TEST_BOARD_COUNT == 4
#include "ml_demo_position.h"
bool webSniffMlDemo = false;
#endif

// Root-only command handlers. Normal nodes continue using the existing mesh protocol.
inline bool webCommandSupported(const String &name) {
  return name=="conf" || name=="sniff" || name=="collect" || name=="discover" || name=="status" || name=="calibrate" || name=="diagnose";
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

void endWebCommand(const char *status, const char *message = nullptr) {
  JsonDocument end;commandEnvelope(end,activeCommandId,"end");end["status"]=status;
  if(message)end["message"]=message;
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
  const bool pairCommand = name=="calibrate" || name=="diagnose";
  if(!pairCommand && (!calibrationMode.networkSynced() || !timeSlicing.networkReadyForConfiguration(macList.size()))) {
    rejectCommand(id,"MESH_NOT_READY","Wait for the configured nodes and A/n acknowledgements.");return;
  }
  if(name=="conf") { startWebConf(id);return; }
  if(name=="sniff" || name=="collect" || name=="discover") {
    const String target=params["mac"] | "";
    const unsigned channel=params["channel"] | 0u;
    const String text=String("sniff ")+target+(channel ? String(" ")+channel : "");
    const auto command=UserCommand::parse(text.c_str());
    if(command.kind!=UserCommand::Kind::Sniff || channel>13) {
      rejectCommand(id,"INVALID_ARGUMENT","Use a 12-digit hexadecimal MAC and channel 0..13 (0 means automatic).");return;
    }
    if(name=="collect" && (TEST_BOARD_COUNT!=4 || channel==0)) {
      rejectCommand(id,"COLLECTION_NOT_READY","Collection requires four boards and a locked channel 1..13.");return;
    }
    if(name=="discover" && channel!=0) {
      rejectCommand(id,"INVALID_ARGUMENT","Channel discovery requires channel zero.");return;
    }
    for(const auto &mac:macList) if(mac==command.target) {
      rejectCommand(id,"INVALID_TARGET","Choose an external Wi-Fi device, not a configured mesh station.");return;
    }
    const bool mlDemo = name=="sniff" && (params["mlDemo"] | false);
    if (mlDemo) {
#if CONFIG_IDF_TARGET_ESP32S3 && TEST_BOARD_COUNT == 4
      if (!lastCoordinateRound || !MlDemo::frame(lastNodeCoordinates).valid) {
        rejectCommand(id,"ML_LAYOUT_REQUIRED","Run Configure with all four nodes in the 1 m demo square.");return;
      }
#else
      rejectCommand(id,"ML_UNAVAILABLE","The synthetic demo requires a four-node ESP32-S3 root build.");return;
#endif
    }
    if(!beginWebLifecycle(id))return;
    webSniffTarget=command.target;
#if CONFIG_IDF_TARGET_ESP32S3 && TEST_BOARD_COUNT == 4
    webSniffMlDemo = mlDemo;
#endif
    if(!timeSlicing.beginSniff(macList,command.target,command.channel,0,false,name=="collect" || mlDemo,name=="discover")) {
      rejectCommand(id,"START_FAILED","Could not start sniffing.");webCommandRunning=false;activeCommandId="";
    }
    return;
  }
  unsigned board=params["board"] | 0u, distance=params["distance"] | 1u;
  if(board<2 || board>TEST_BOARD_COUNT || distance<1 || distance>5) {
    rejectCommand(id,"INVALID_ARGUMENT","Select a configured normal board and distance 1..5 m.");return;
  }
  if(!calibrationMode.modelAcknowledged(board)) {
    char message[96];
    snprintf(message, sizeof(message), "Waiting for selected board %02X to join and acknowledge the active A/n.", board);
    rejectCommand(id,"MESH_NOT_READY",message);return;
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

#if CONFIG_IDF_TARGET_ESP32S3 && TEST_BOARD_COUNT == 4
void appendMlDemoResult(JsonObject demo, const char *target) {
  demo["status"] = "unavailable";
  if (webCancelled) {
    demo["message"] = "Synthetic ML demo cancelled.";
    return;
  }
  const auto frame = MlDemo::frame(lastNodeCoordinates);
  if (!frame.valid) {
    demo["message"] = "Run Configure with all four nodes to establish the demo frame.";
    return;
  }
  String macs[4];
  for (unsigned i = 0; i < 4; ++i) macs[i] = configuredMac(i + 1);
  unsigned used = 0;
  uint32_t inferenceUs = 0;
  const uint32_t heapBefore = ESP.getFreeHeap();
  double x = 0, y = 0;
  for (unsigned window = 1; window <= TimeSliceProtocol::SNIFF_CYCLES; ++window) {
    const JsonObjectConst reports = sniffCycleReports[String(window)].as<JsonObjectConst>();
    float features[13] = {};
    features[12] = 1.0f;
    bool complete = true;
    for (unsigned i = 0; i < 4; ++i) {
      const JsonObjectConst report = reports[macs[i]].as<JsonObjectConst>();
      const JsonVariantConst mean = report["RSSI"][target], stddev = report["RSSIStdDb"];
      const unsigned count = report["Samples"][target] | 0u;
      if (strcmp(report["Status"] | "", "ok") != 0 ||
          report["SniffMs"].as<unsigned>() != TimeSliceProtocol::COLLECTION_US / 1000 ||
          count < 20 || !mean.is<float>() || !stddev.is<float>()) {
        complete = false;
        break;
      }
      features[i] = mean.as<float>();
      features[i + 4] = stddev.as<float>();
      features[i + 8] = count;
      if (!std::isfinite(features[i]) || features[i] >= 0 ||
          !std::isfinite(features[i + 4]) || features[i + 4] < 0) {
        complete = false;
        break;
      }
    }
    if (!complete) continue;
    const uint32_t started = micros();
    const float predictedX = localization_x_predict(features, 13);
    const float predictedY = localization_y_predict(features, 13);
    inferenceUs += micros() - started;
    if (!std::isfinite(predictedX) || !std::isfinite(predictedY)) continue;
    x += predictedX;
    y += predictedY;
    ++used;
  }
  const uint32_t heapAfter = ESP.getFreeHeap();
  demo["validWindows"] = used;
  demo["windows"] = TimeSliceProtocol::SNIFF_CYCLES;
  if (!used) {
    demo["message"] = "The demo needs a 10-second window with at least 20 packets from each of the four nodes.";
    return;
  }
  const auto point = frame.map(x / used, y / used, lastNodeCoordinates.points[0]);
  demo["status"] = "estimated";
  demo["position"]["x"] = point.x;
  demo["position"]["y"] = point.y;
  demo["inferenceUs"] = inferenceUs;
  demo["heapBeforeBytes"] = heapBefore;
  demo["heapAfterBytes"] = heapAfter;
  demo["message"] = "Synthetic 1 m model. Field accuracy has not been validated.";
  FW_LOG("[ML DEMO] (%0.3f, %0.3f) m from %u windows, %lu us, heap %lu -> %lu bytes",
         point.x, point.y, used, (unsigned long)inferenceUs,
         (unsigned long)heapBefore, (unsigned long)heapAfter);
}
#endif

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
#if CONFIG_IDF_TARGET_ESP32S3 && TEST_BOARD_COUNT == 4
  if (webSniffMlDemo) appendMlDemoResult(data["mlDemo"].to<JsonObject>(), target);
#endif
  queueCommandEvent(data);
  if(!webCancelled && !timeSlicing.captureFailure().length() && !validNodes) {
    JsonDocument error;commandEnvelope(error,activeCommandId,"error");error["code"]="TARGET_NOT_HEARD";
    error["message"]="No valid target averages. Check the Wi-Fi MAC, 2.4 GHz traffic, activity, and range.";queueCommandEvent(error);
  }
  endWebCommand(webCancelled ? "cancelled" : completeNodes==TEST_BOARD_COUNT ? "success" : validNodes ? "partial" : "failed");
}

void finishWebCollection() {
  char target[18];snprintf(target,sizeof(target),"%02X:%02X:%02X:%02X:%02X:%02X",
      webSniffTarget[0],webSniffTarget[1],webSniffTarget[2],webSniffTarget[3],webSniffTarget[4],webSniffTarget[5]);
  unsigned complete=0;
  for(unsigned window=1;window<=3;++window) {
    JsonDocument data;commandEnvelope(data,activeCommandId,"data");
    data["kind"]="collection_window";data["window"]=window;
    data["targetMac"]=target;data["channel"]=timeSlicing.discoveredChannel;
    data["durationMs"]=TimeSliceProtocol::COLLECTION_US/1000;
    data["A"]=A;data["n"]=n;
    JsonArray receivers=data["receivers"].to<JsonArray>();
    bool good=true;
    for(unsigned board=1;board<=4;++board) {
      const String mac=configuredMac(board);
      JsonVariantConst report=sniffCycleReports[String(window)][mac];
      JsonObject row=receivers.add<JsonObject>();row["board"]=board;row["mac"]=mac;
      row["status"]=report["Status"] | "missing";
      row["roundId"]=report["RoundId"];
      row["channel"]=report["SniffChannel"];
      row["durationMs"]=report["SniffMs"];
      row["observedMs"]=report["ObservedWindowMs"];
      row["packetSpanMs"]=report["PacketSpanMs"];
      row["count"]=report["Samples"][target];
      row["mean"]=report["RSSI"][target];row["std"]=report["RSSIStdDb"];
      good &= strcmp(report["Status"] | "", "ok")==0 && report["Samples"][target].as<unsigned>()>=20;
    }
    if(good)++complete;
    queueCommandEvent(data);
  }
  endWebCommand(webCancelled ? "cancelled" : complete==3 ? "success" : "partial");
}

void publishCaptureFailure() {
  if (webCancelled || !timeSlicing.captureFailure().length()) return;
  JsonDocument event;
  commandEnvelope(event,activeCommandId,"error");
  event["code"]="CAPTURE_MESH_FAILURE";
  event["message"]=timeSlicing.captureFailure();
  queueCommandEvent(event);
}

void updateWebCommand() {
  processWebControl();
  if(!webCommandRunning)return;
  const auto phase=timeSlicing.phase();
  if(activeCommandName=="conf" || activeCommandName=="sniff" || activeCommandName=="collect" || activeCommandName=="discover") {
    if(timeSlicing.busy()) {
      if(backendServiceAllowed() && commandEvents.room(TEST_BOARD_COUNT+6)) {
        String message=timeSlicing.reportWaitMessage();
        if(message.length() && timeSlicing.captureCycle())
          message=String("Cycle ")+timeSlicing.captureCycle()+": "+message;
        if(message.length() && message!=webSnapshot) {
          webSnapshot=message;
          JsonDocument event;commandEnvelope(event,activeCommandId,"log");
          event["message"]=message;
          queueCommandEvent(event);
        }
      }
      return;
    }
    if(activeCommandName=="conf") { finishWebConf(phase);webCommandRunning=false; }
    else if(activeCommandName=="collect") { publishCaptureFailure(); finishWebCollection(); }
    else if(activeCommandName=="discover") {
      publishCaptureFailure();
      JsonDocument event;commandEnvelope(event,activeCommandId,"data");event["kind"]="channel_discovery";
      event["channel"]=timeSlicing.discoveredChannel;queueCommandEvent(event);
      endWebCommand(webCancelled ? "cancelled" : timeSlicing.discoveredChannel &&
                    !timeSlicing.captureFailure().length() ? "success" : "failed");
    }
    else { publishCaptureFailure(); finishWebSniff(); }
    return;
  }
  if(activeCommandName=="calibrate" || activeCommandName=="diagnose") {
    const bool finished=!calibrationMode.localSession();
    if(!finished && (!backendServiceAllowed() || !commandEvents.room(5)))return;
    JsonDocument snapshot;calibrationMode.snapshot(snapshot,activeCommandName=="diagnose");
    String serialized;serializeJson(snapshot,serialized);
    if(serialized!=webSnapshot || finished) {
      webSnapshot=serialized;commandEnvelope(snapshot,activeCommandId,"data");queueCommandEvent(snapshot);
    }
    if(finished) {
      const char *outcome=calibrationMode.outcome();
      const bool partial=strcmp(outcome,"partial")==0;
      if(activeCommandName=="calibrate" && (strcmp(outcome,"success")==0 || partial)) {
        lastNodeCoordinates={};lastCoordinateRound=0;
      }
      endWebCommand(outcome,partial ? snapshot["message"].as<const char *>() : nullptr);
    }
  }
}
