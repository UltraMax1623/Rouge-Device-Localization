#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "rssi_distance.h"

namespace Calibration {
// Histogram avoids allocating or sorting packet buffers in the Wi-Fi callback.
struct Histogram {
  std::array<uint32_t, 128> bins{};
  uint32_t count = 0;
  void add(int rssi) {
    if (rssi >= -127 && rssi <= 0) { ++bins[-rssi]; ++count; }
  }
  double spread() const {
    if (!count) return NAN;
    double sum = 0, squares = 0;
    for (unsigned i = 0; i < bins.size(); ++i) {
      sum -= double(i) * bins[i];
      squares += double(i) * i * bins[i];
    }
    return std::sqrt(std::max(0.0, squares / count - (sum / count) * (sum / count)));
  }
  double trimmedMean() const {
    if (!count) return NAN;
    const uint32_t trim = count / 10;
    uint32_t rank = 0, kept = 0;
    double sum = 0;
    for (unsigned i = 0; i < bins.size(); ++i) {
      const uint32_t end = rank + bins[i];
      const uint32_t lo = std::max(rank, trim);
      const uint32_t hi = std::min(end, count - trim);
      if (hi > lo) { kept += hi - lo; sum -= double(i) * (hi - lo); }
      rank = end;
    }
    return kept ? sum / kept : NAN;
  }
};

struct Fit {
  double a = NAN, n = NAN, rmse = NAN;
  bool valid() const {
    // Acceptance gates, not a claim about universal indoor parameters.
    return std::isfinite(a) && a >= -100 && a <= -10 &&
        std::isfinite(n) && n >= 0.5 && n <= 6 &&
        std::isfinite(rmse) && rmse <= 4;
  }
};
inline Fit fit(const std::array<double, 5> &rssi) {
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (unsigned i = 0; i < rssi.size(); ++i) {
    if (!std::isfinite(rssi[i]) || rssi[i] < -127 || rssi[i] > 0) return {};
    const double x = std::log10(double(i + 1));
    sx += x; sy += rssi[i]; sxx += x * x; sxy += x * rssi[i];
  }
  const double slope = (5 * sxy - sx * sy) / (5 * sxx - sx * sx);
  Fit result;
  result.n = -slope / 10;
  result.a = (sy - slope * sx) / 5;
  double error = 0;
  for (unsigned i = 0; i < rssi.size(); ++i) {
    const double residual = rssi[i] - (result.a + slope * std::log10(double(i + 1)));
    error += residual * residual;
  }
  result.rmse = std::sqrt(error / 5);
  return result;
}

struct TrainingCheck {
  std::array<double, 5> forward{}, reverse{};
  std::array<bool, 5> recheck{};
  bool passed = true;
};
inline TrainingCheck checkTraining(const Fit &model,
    const std::array<double, 5> &forward, const std::array<double, 5> &reverse) {
  TrainingCheck result;
  for (unsigned i = 0; i < 5; ++i) {
    const double actual = i + 1;
    result.forward[i] = rssiToDistance(forward[i], model.a, model.n);
    result.reverse[i] = rssiToDistance(reverse[i], model.a, model.n);
    result.recheck[i] = !std::isfinite(result.forward[i]) || !std::isfinite(result.reverse[i]) ||
        std::fabs(result.forward[i] - actual) > 0.30 * actual ||
        std::fabs(result.reverse[i] - actual) > 0.30 * actual;
    if (result.recheck[i]) result.passed = false;
  }
  return result;
}

struct ValidationCheck {
  double forward = NAN, reverse = NAN, previousForward = NAN, previousReverse = NAN;
  double mae = NAN, previousMae = NAN;
  bool withinTolerance = false, noRegression = false;
  bool passed() const { return withinTolerance && noRegression; }
};
inline ValidationCheck checkValidation(const Fit &candidate, double oldA, double oldN,
                                      double forward, double reverse) {
  constexpr double actual = 2.5;
  ValidationCheck result;
  result.forward = rssiToDistance(forward, candidate.a, candidate.n);
  result.reverse = rssiToDistance(reverse, candidate.a, candidate.n);
  result.previousForward = rssiToDistance(forward, oldA, oldN);
  result.previousReverse = rssiToDistance(reverse, oldA, oldN);
  result.mae = (std::fabs(result.forward - actual) + std::fabs(result.reverse - actual)) / 2;
  result.previousMae = (std::fabs(result.previousForward - actual) +
                        std::fabs(result.previousReverse - actual)) / 2;
  result.withinTolerance = std::isfinite(result.mae) &&
      std::fabs(result.forward - actual) <= 0.30 * actual &&
      std::fabs(result.reverse - actual) <= 0.30 * actual;
  // A 2 cm allowance avoids rejecting a numerically equivalent model.
  result.noRegression = std::isfinite(result.previousMae) && std::isfinite(result.mae) &&
      result.mae <= result.previousMae + 0.02;
  return result;
}

struct WindowSummary {
  unsigned count = 0;
  double mean = NAN, minimum = NAN, maximum = NAN, spread = NAN;
};
inline WindowSummary summarizeWindows(const std::array<double, 5> &values,
                                      const std::array<bool, 5> &valid) {
  WindowSummary result;
  double sum = 0;
  for (unsigned i = 0; i < values.size(); ++i) {
    if (!valid[i] || !std::isfinite(values[i])) continue;
    if (!result.count) result.minimum = result.maximum = values[i];
    result.minimum = std::min(result.minimum, values[i]);
    result.maximum = std::max(result.maximum, values[i]);
    sum += values[i]; ++result.count;
  }
  if (!result.count) return result;
  result.mean = sum / result.count;
  double squares = 0;
  for (unsigned i = 0; i < values.size(); ++i)
    if (valid[i] && std::isfinite(values[i])) squares += (values[i] - result.mean) * (values[i] - result.mean);
  result.spread = std::sqrt(squares / result.count);
  return result;
}
}  // namespace Calibration
