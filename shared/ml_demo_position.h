#pragma once

#include "coordinate_engine.h"
#include "ml_demo_models/localization_x.h"
#include "ml_demo_models/localization_y.h"

namespace MlDemo {
// Model coordinates use metres, with 01 as origin, 02 along +X and 03 along +Y.
// Configure estimates a relative frame from RSSI. Use its orientation without
// scaling the model's metre outputs by the noisy estimated anchor distances.
struct Frame {
  double ux = 0, uy = 0, handedness = 1;
  bool valid = false;

  Coordinates::Point map(float x, float y, const Coordinates::Point &origin) const {
    return {origin.x + x * ux - y * uy * handedness,
            origin.y + x * uy + y * ux * handedness, true};
  }
};

inline Frame frame(const Coordinates::Result &layout) {
  if (!layout.valid) return {};
  for (unsigned i = 0; i < 4; ++i) {
    const auto &point = layout.points[i];
    if (!point.valid || !std::isfinite(point.x) || !std::isfinite(point.y)) return {};
  }
  const auto &origin = layout.points[0];
  const double dx = layout.points[1].x - origin.x;
  const double dy = layout.points[1].y - origin.y;
  const double length = std::hypot(dx, dy);
  if (length < 1e-6) return {};
  const double ux = dx / length, uy = dy / length;
  const double vx = layout.points[2].x - origin.x;
  const double vy = layout.points[2].y - origin.y;
  const double cross = ux * vy - uy * vx;
  if (std::fabs(cross) < 1e-6 || std::fabs(cross) < 0.1 * std::hypot(vx, vy)) return {};
  return {ux, uy, cross < 0 ? -1.0 : 1.0, true};
}
}
