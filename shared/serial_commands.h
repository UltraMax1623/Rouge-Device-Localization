#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace UserCommand {
enum class Kind { Invalid, Conf, Sniff };
struct Command {
  Kind kind = Kind::Invalid;
  std::array<uint8_t, 6> target{};
  uint8_t channel = 0;  // 0: scan channels 1..11.
};
class LineReader {
 public:
  enum class Result { None, Line, Busy, Invalid };
  const char *text() const { return line_; }
  Result push(char value, bool busy) {
    blocked_ |= busy;
    if (value == '\r' || value == '\n') {
      const bool any = length_ || invalid_;
      line_[length_] = 0;
      const auto result = !any ? Result::None : blocked_ ? Result::Busy
          : invalid_ ? Result::Invalid : Result::Line;
      length_ = 0;
      invalid_ = blocked_ = false;
      return result;
    }
    if (!invalid_) {
      if (length_ + 1 < sizeof(line_) &&
          ((value >= 32 && value <= 126) || value == '\t')) line_[length_++] = value;
      else invalid_ = true;
    }
    return Result::None;
  }
 private:
  char line_[64]{};
  size_t length_ = 0;
  bool invalid_ = false, blocked_ = false;
};
inline int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}
inline Command parse(const char *text) {
  Command command;
  while (*text == ' ' || *text == '\t') ++text;
  if (strncmp(text, "conf", 4) == 0) {
    text += 4;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == 0) command.kind = Kind::Conf;
    return command;
  }
  if (strncmp(text, "sniff ", 6) != 0 && strncmp(text, "sniff\t", 6) != 0)
    return command;
  text += 6;
  while (*text == ' ' || *text == '\t') ++text;
  for (size_t i = 0; i < 6; ++i) {
    if (!text[0] || !text[1]) return Command{};
    const int high = hexDigit(text[0]), low = hexDigit(text[1]);
    if (high < 0 || low < 0) return Command{};
    command.target[i] = uint8_t((high << 4) | low);
    text += 2;
  }
  if (*text && *text != ' ' && *text != '\t') return Command{};
  while (*text == ' ' || *text == '\t') ++text;
  if (*text) {
    unsigned channel = 0;
    if (*text < '0' || *text > '9') return Command{};
    while (*text >= '0' && *text <= '9') {
      channel = channel * 10 + unsigned(*text++ - '0');
      if (channel > 13) return Command{};
    }
    if (channel < 1) return Command{};
    command.channel = uint8_t(channel);
    while (*text == ' ' || *text == '\t') ++text;
    if (*text) return Command{};
  }
  command.kind = Kind::Sniff;
  return command;
}
}  // namespace UserCommand
