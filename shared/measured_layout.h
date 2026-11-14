#pragma once
#include "coordinate_engine.h"
#include <initializer_list>

namespace Coordinates {
inline Result measuredSquare(double side) {
  if (!std::isfinite(side) || side <= 0) return {};
  Result layout;
  layout.points[0] = {0, 0, true};
  layout.points[1] = {side, 0, true};
  layout.points[2] = {0, side, true};
  layout.points[3] = {side, side, true};
  layout.valid = true;
  return layout;
}

// Perimeter 01-02-04-03, split into triangles along 01-04.
inline Result measuredQuadrilateral(double a, double b, double c, double d, double diagonal) {
  double scale = 0;
  for (double length : {a, b, c, d, diagonal}) {
    if (!std::isfinite(length) || length <= 0) return {};
    if (length > scale) scale = length;
  }
  a /= scale; b /= scale; c /= scale; d /= scale; diagonal /= scale;
  if (diagonal <= std::fabs(a-b) || diagonal >= a+b ||
      diagonal <= std::fabs(d-c) || diagonal >= d+c) return {};
  const double u = (a*a - b*b + diagonal*diagonal) / (2*diagonal);
  const double v = (d*d - c*c + diagonal*diagonal) / (2*diagonal);
  const double h2 = a*a - u*u, k2 = d*d - v*v;
  if (h2 <= 0 || k2 <= 0) return {};
  const double h = std::sqrt(h2), k = std::sqrt(k2);
  const double cross = u*k + h*v;
  // The other diagonal must cross 01-04 between its endpoints.
  if (cross <= 0 || cross >= diagonal * (h+k)) return {};
  Result layout;
  layout.points[0] = {0, 0, true};
  layout.points[1] = {a*scale, 0, true};
  layout.points[2] = {(u*v-h*k)/a*scale, cross/a*scale, true};
  layout.points[3] = {diagonal*u/a*scale, diagonal*h/a*scale, true};
  for (unsigned i = 0; i < 4; ++i) {
    if (!std::isfinite(layout.points[i].x) || !std::isfinite(layout.points[i].y)) return {};
  }
  layout.valid = true;
  return layout;
}
}
