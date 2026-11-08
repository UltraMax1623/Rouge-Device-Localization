#pragma once
#include <math.h>
#include "generated/dummy_x.h"
#include "generated/dummy_y.h"
#include "generated/parity_vectors.h"

// Outputs are relative to square side length, with the same origin as labels.
// Do not clamp to [0,1]: the dataset deliberately includes outside positions.
inline bool dummyCasePasses(int index, float &x, float &y) {
  x = dummy_x_predict(kInputs[index], kFeatureCount);
  y = dummy_y_predict(kInputs[index], kFeatureCount);
  return isfinite(x) && isfinite(y) &&
         fabsf(x - kExpected[index][0]) <= 1e-5f &&
         fabsf(y - kExpected[index][1]) <= 1e-5f;
}
