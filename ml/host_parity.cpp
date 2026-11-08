#include <stdio.h>
#include "esp32_dummy/self_test.h"

int main() {
  int failures = 0;
  for (int i = 0; i < kCaseCount; ++i) {
    float x, y;
    if (!dummyCasePasses(i, x, y)) {
      ++failures;
      printf("FAIL case %d: %.8f %.8f\n", i, x, y);
    }
  }
  printf("SYNTHETIC Python/C++ parity: %d/%d passed\n",
         kCaseCount - failures, kCaseCount);
  return failures ? 1 : 0;
}
