#pragma once

// Shared transport for future command handlers. Keep events until backend ACK.
class CommandEvents {
 public:
  bool push(JsonDocument &event) {
    if (count_ == 24) return false;
    event["sequence"] = ++sequence_;
    Item &slot = items_[(head_ + count_) % 24];
    slot.commandId=event["commandId"].as<String>();
    slot.command=event["command"].as<String>();
    slot.sequence=sequence_;
    slot.payload="";
    serializeJson(event, slot.payload);
    ++count_;
    return true;
  }
  bool available() const { return count_ != 0; }
  bool room(size_t needed) const { return count_ + needed <= 24; }
  void describePending(JsonDocument &state) const {
    if (!count_) return;
    state["retainedCommandId"]=items_[head_].commandId;
    state["retainedCommand"]=items_[head_].command;
  }
  bool send(WebSocketsClient &socket, bool force = false) {
    if (!count_ || (!force && millis() - lastSend_ < 1500)) return false;
    lastSend_ = millis();
    return socket.sendTXT(items_[head_].payload);
  }
  void acknowledge(JsonDocument &ack) {
    if (!count_) return;
    const Item &front=items_[head_];
    if (ack["commandId"].as<String>() != front.commandId ||
        ack["sequence"].as<uint32_t>() != front.sequence) return;
    items_[head_] = Item{};
    head_ = (head_ + 1) % 24;
    --count_;
    lastSend_ = millis() - 1500;
  }
 private:
  struct Item { String payload,commandId,command; uint32_t sequence=0; };
  Item items_[24];
  uint8_t head_ = 0, count_ = 0;
  uint32_t sequence_ = 0, lastSend_ = 0;
};
