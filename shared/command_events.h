#pragma once

// Shared transport for future command handlers. Keep events until backend ACK.
class CommandEvents {
 public:
  bool push(JsonDocument &event) {
    if (count_ == 24) return false;
    event["sequence"] = ++sequence_;
    String &slot = items_[(head_ + count_) % 24];
    slot = "";
    serializeJson(event, slot);
    ++count_;
    return true;
  }
  bool available() const { return count_ != 0; }
  bool room(size_t needed) const { return count_ + needed <= 24; }
  void describePending(JsonDocument &state) const {
    if (!count_ || state["commandId"].as<String>().length()) return;
    JsonDocument front;
    if (deserializeJson(front, items_[head_])) return;
    state["commandId"]=front["commandId"];
    state["command"]=front["command"];
  }
  bool send(WebSocketsClient &socket, bool force = false) {
    if (!count_ || (!force && millis() - lastSend_ < 1500)) return false;
    lastSend_ = millis();
    return socket.sendTXT(items_[head_]);
  }
  void acknowledge(JsonDocument &ack) {
    if (!count_) return;
    JsonDocument front;
    deserializeJson(front, items_[head_]);
    if (ack["commandId"].as<String>() != front["commandId"].as<String>() ||
        ack["sequence"].as<uint32_t>() != front["sequence"].as<uint32_t>()) return;
    items_[head_] = "";
    head_ = (head_ + 1) % 24;
    --count_;
    lastSend_ = millis() - 1500;
  }
 private:
  String items_[24];
  uint8_t head_ = 0, count_ = 0;
  uint32_t sequence_ = 0, lastSend_ = 0;
};
