#pragma once
#include <cmath>
#include <cstddef>

namespace Coordinates {
constexpr size_t MAX_NODES = 15;
struct Point { double x = 0, y = 0; bool valid = false; };
struct Result { Point points[MAX_NODES]; double rmse = 0; bool valid = false; };
// Input is a symmetric matrix of averaged directional ranges; zero means absent.
// Root is the origin; the first remaining node defines positive X. Distances
// determine orientation only up to rotation/reflection, never compass direction.
inline Result solve(const double d[MAX_NODES][MAX_NODES], const bool present[MAX_NODES], size_t count) {
  Result out;
  if (!count || count > MAX_NODES || !present[0]) return out;
  out.points[0].valid = true;
  size_t ids[MAX_NODES], n = 0;
  for (size_t i = 0; i < count; ++i) if (present[i]) ids[n++] = i;
  if (n < 2) return out;
  for (size_t i = 0; i < n; ++i) for (size_t j = i+1; j < n; ++j)
    if (!std::isfinite(d[ids[i]][ids[j]]) || d[ids[i]][ids[j]] <= 0) return out;
  const double base = d[0][ids[1]];
  double best = 1e300;
  for (unsigned seed = 0; seed < 16; ++seed) {
    Point p[MAX_NODES];
    p[1].x = base;
    for (size_t i = 2; i < n; ++i) {
      const double a = d[0][ids[i]], b = d[ids[1]][ids[i]];
      p[i].x = (a*a + base*base - b*b)/(2*base);
      const double h = a*a - p[i].x*p[i].x;
      p[i].y = std::sqrt(h > base*base*1e-6 ? h : base*base*1e-6);
      if (i > 2 && ((seed * 2654435761u) >> (i % 16)) & 1u) p[i].y *= -1;
    }
    auto cost = [&](Point *q) {
      double sum = 0;
      for (size_t i = 0; i < n; ++i) for (size_t j = i+1; j < n; ++j) {
        double e = std::hypot(q[i].x-q[j].x,q[i].y-q[j].y)-d[ids[i]][ids[j]];
        sum += e*e;
      }
      return sum;
    };
    double score = cost(p);
    for (unsigned iter = 0; iter < 350; ++iter) {
      Point gradient[MAX_NODES], candidate[MAX_NODES];
      for (size_t i = 0; i < n; ++i) for (size_t j = i+1; j < n; ++j) {
        double dx=p[i].x-p[j].x, dy=p[i].y-p[j].y, len=std::hypot(dx,dy);
        if (len < 1e-12) continue;
        double f=2*(len-d[ids[i]][ids[j]])/len;
        gradient[i].x+=f*dx; gradient[i].y+=f*dy;
        gradient[j].x-=f*dx; gradient[j].y-=f*dy;
      }
      double step=0.3/n, next=score;
      for (unsigned attempt=0; attempt<15; ++attempt) {
        for (size_t i=0;i<n;++i) candidate[i]=p[i];
        for (size_t i=1;i<n;++i) {
          candidate[i].x-=step*gradient[i].x;
          if (i>1) candidate[i].y-=step*gradient[i].y;
        }
        if (candidate[1].x < 1e-9) candidate[1].x=1e-9;
        next=cost(candidate);
        if (next <= score) break;
        step*=0.5;
      }
      if (next > score || score-next < 1e-14) break;
      for (size_t i=0;i<n;++i) p[i]=candidate[i];
      score=next;
    }
    if (score < best) {
      best=score;
      double ysum=0; for(size_t i=2;i<n;++i) ysum+=p[i].y;
      for(size_t i=0;i<n;++i) out.points[ids[i]]={p[i].x, ysum<0 ? -p[i].y : p[i].y, true};
    }
  }
  out.rmse=std::sqrt(best/(n*(n-1)/2)); out.valid=true;
  return out;
}
}
