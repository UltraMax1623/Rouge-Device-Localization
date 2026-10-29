#pragma once

#include <Arduino.h>
#include "../root_node/time_slicing_protocol.h"

// Override these before including node_firmware.h for a different board.
// RGB_BUILTIN comes from the selected Arduino board variant. MODE_RGB_PIN
// can override it with the actual RGB data GPIO (e.g. 38 on some S3 boards).
#ifndef MODE_RGB_PIN
#if defined(RGB_BUILTIN) && !defined(MODE_LED_PIN)
#define MODE_RGB_PIN RGB_BUILTIN
#endif
#endif
#ifndef MODE_LED_PIN
#ifdef LED_BUILTIN
#define MODE_LED_PIN LED_BUILTIN
#else
#define MODE_LED_PIN 2  // Common ESP32 DevKit LED; check your board schematic.
#endif
#endif
#ifndef MODE_LED_ACTIVE_LOW
#define MODE_LED_ACTIVE_LOW 0
#endif

inline void writeModeLed(bool on, uint8_t pattern = 0) {
#if defined(MODE_RGB_PIN)
  // Low brightness: mesh green, broadcast cyan, conf yellow, discovery purple,
  // TX orange, RX blue, waiting off and fault red. Monochrome uses blink/steady/off.
  const uint8_t red = on && (pattern == 1 || pattern == 3 || pattern == 5 || pattern == 6) ? 16 : 0;
  const uint8_t green = !on ? 0 : pattern == 6 ? 8 :
      (pattern == 0 || pattern == 1 || pattern == 4) ? 16 : 0;
  const uint8_t blue = on && (pattern == 2 || pattern == 4 || pattern == 5 || pattern == 7) ? 16 : 0;
  rgbLedWrite(MODE_RGB_PIN, red, green, blue);
#else
  digitalWrite(MODE_LED_PIN, (on != bool(MODE_LED_ACTIVE_LOW)) ? HIGH : LOW);
#endif
}

inline void beginModeIndicator() {
#if defined(MODE_RGB_PIN)
  unsigned physicalPin = MODE_RGB_PIN;
#if defined(RGB_BUILTIN)
  // The Arduino core exposes RGB_BUILTIN as a virtual pin on some boards.
  // Report the real data GPIO using the same mapping as rgbLedWrite().
  if (physicalPin == RGB_BUILTIN) physicalPin -= SOC_GPIO_PIN_COUNT;
#endif
  Serial.printf("[LED] RGB driver, data GPIO %u\n", physicalPin);
  Serial.println("[LED] If dark, check the selected board and its RGB data-pin revision");
#else
  pinMode(MODE_LED_PIN, OUTPUT);
  Serial.printf("[LED] Monochrome driver, GPIO %u (active %s)\n",
                unsigned(MODE_LED_PIN), MODE_LED_ACTIVE_LOW ? "low" : "high");
#endif
  writeModeLed(false);
}

// Nonblocking; called every loop so mesh maintenance and slot timing continue.
inline void updateModeIndicator(const String &mode, bool fault,
                                bool broadcasting,
                                TimeSliceProtocol::RadioActivity radio = TimeSliceProtocol::RadioActivity::None) {
  using TimeSliceProtocol::RadioActivity;
  const uint8_t pattern = fault ? 3 : radio == RadioActivity::Transmit ? 6
      : radio == RadioActivity::Receive ? 7 : radio == RadioActivity::Waiting ? 8
      : mode == "conf" ? 1 : mode == "sniff" ? 2
                                      : mode == "scan" ? 5 : broadcasting ? 4 : 0;
  static uint8_t previous = 255;
  static uint32_t since = 0;
  const uint32_t now = millis();
  if (pattern != previous) {
    previous = pattern;
    since = now;
    Serial.printf("[LED] %s\n", fault ? "fault" : pattern == 6 ? "TX - measurement slot"
        : pattern == 7 ? "RX - listening" : pattern == 8 ? "waiting - no active measurement slot"
        : pattern == 4
                                             ? "mesh broadcast" : mode.c_str());
  }
  const uint32_t elapsed = now - since;
  bool on;
  if (pattern == 8) {
    on = false;
  } else if (pattern == 6) {
    on = (elapsed / 100) % 2 == 0;
  } else if (pattern == 3 || pattern == 2) {
    const uint32_t part = elapsed % 2000;
    on = part < 100 || (part >= 250 && part < 350) ||
         (pattern == 3 && part >= 500 && part < 600);
  } else if (pattern == 1) {
    on = (elapsed / 500) % 2 == 0;
  } else if (pattern == 4) {
    on = (elapsed / 60) % 2 == 0;
  } else if (pattern == 5) {
    on = (elapsed / 200) % 2 == 0;
  } else {
    on = true;
  }
  static int previousOutput = -1;
  const int output = on ? int(pattern) + 1 : 0;
  if (previousOutput != output) {
    writeModeLed(on, pattern);
    previousOutput = output;
  }
}
