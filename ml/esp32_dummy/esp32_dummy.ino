// Standalone test: temporarily replaces the firmware on the selected board.
// Not connected to the mesh and not a real localization model.
#include <Arduino.h>
#include "self_test.h"

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("SYNTHETIC ML deployment test; no field accuracy claims");
  const uint32_t heapBefore = ESP.getFreeHeap();
  int failures = 0;
  uint32_t elapsed = 0;
  for (int i = 0; i < kCaseCount; ++i) {
    float x, y;
    const uint32_t start = micros();
    const bool passed = dummyCasePasses(i, x, y);
    elapsed += micros() - start;
    if (!passed) ++failures;
    if (i == 0) {
      const float side = kInputs[i][kFeatureCount - 1];
      Serial.printf("Example: x=%.4f m, y=%.4f m, side=%.1f m\n", x*side, y*side, side);
    }
  }
  Serial.printf("PARITY %s: %d/%d passed\n", failures ? "FAIL" : "PASS",
                kCaseCount-failures, kCaseCount);
  Serial.printf("Mean two-axis test time (includes comparison): %.2f us\n",
                float(elapsed)/kCaseCount);
  Serial.printf("Free heap before=%lu after=%lu bytes\n",
                (unsigned long)heapBefore, (unsigned long)ESP.getFreeHeap());
}

void loop() { delay(1000); }
