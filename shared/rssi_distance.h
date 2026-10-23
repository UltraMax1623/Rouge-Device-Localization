#pragma once

#include <cmath>
#include <limits>

// Path-loss model parameters copied from Localization/Localization.ino.
// A is the reference RSSI at one metre; n is the path-loss exponent.
extern double A;
extern double n;

struct DistanceAverage {
  double sum = 0;
  unsigned count = 0;
  void add(double metres) {
    if (std::isfinite(metres) && metres > 0) { sum += metres; ++count; }
  }
  double mean() const {
    return count ? sum / count : std::numeric_limits<double>::quiet_NaN();
  }
};

inline double rssiToDistance(double rssi, double referenceRssi,
                             double pathLossExponent) {
  if (!std::isfinite(rssi) || !std::isfinite(referenceRssi) ||
      !std::isfinite(pathLossExponent) || pathLossExponent <= 0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double metres = std::pow(10.0,
      (referenceRssi - rssi) / (10.0 * pathLossExponent));
  return std::isfinite(metres) && metres > 0
      ? metres : std::numeric_limits<double>::quiet_NaN();
}
