#pragma once
#include "coordinate_engine.h"

namespace Coordinates {
struct Target { Point point; unsigned anchors=0; double rmse=0; bool ambiguous=false; };
inline Target locateTarget(const Result &layout, const double *ranges, size_t count) {
  Target result;
  size_t ids[MAX_NODES];
  if (!layout.valid || count>MAX_NODES) return result;
  for(size_t i=0;i<count;++i) if(layout.points[i].valid && std::isfinite(ranges[i]) && ranges[i]>0)
    ids[result.anchors++]=i;
  if(!result.anchors) return result;
  result.ambiguous=result.anchors<3;
  auto cost=[&](double x,double y) {
    double total=0;
    for(unsigned k=0;k<result.anchors;++k) { const auto &a=layout.points[ids[k]];
      double e=std::hypot(x-a.x,y-a.y)-ranges[ids[k]]; total+=e*e; }
    return total;
  };
  const auto &a=layout.points[ids[0]];
  double initialX=a.x+ranges[ids[0]]/std::sqrt(2.0), initialY=a.y+ranges[ids[0]]/std::sqrt(2.0);
  if(result.anchors>=2) {
    const auto &b=layout.points[ids[1]];
    double dx=b.x-a.x,dy=b.y-a.y,base=std::hypot(dx,dy);
    if(base<1e-9) { result.ambiguous=true; return result; }
    double x=(ranges[ids[0]]*ranges[ids[0]]+base*base-ranges[ids[1]]*ranges[ids[1]])/(2*base);
    double h=ranges[ids[0]]*ranges[ids[0]]-x*x;
    double y=std::sqrt(h>0?h:0);
    initialX=a.x+x*dx/base-y*dy/base; initialY=a.y+x*dy/base+y*dx/base;
    // Nearly collinear anchors leave a reflection ambiguity even with 3+ nodes.
    if(result.anchors>=3) {
      bool nonCollinear=false;
      for(unsigned k=2;k<result.anchors;++k) { const auto &c=layout.points[ids[k]];
        if(std::fabs(dx*(c.y-a.y)-dy*(c.x-a.x))>1e-5*base*base) nonCollinear=true; }
      result.ambiguous=!nonCollinear;
    }
  }
  double best=1e300;
  for(unsigned seed=0;seed<4;++seed) {
    double x=initialX,y=seed&1 ? -initialY : initialY;
    if(result.ambiguous) { x=std::fabs(x); y=std::fabs(y); }
    if(seed>=2) { x+=0.05*ranges[ids[0]]; y+=0.05*ranges[ids[0]]; }
    double score=cost(x,y);
    for(unsigned iter=0;iter<300;++iter) {
      double gx=0,gy=0;
      for(unsigned k=0;k<result.anchors;++k) { const auto &anchor=layout.points[ids[k]];
        double dx=x-anchor.x,dy=y-anchor.y,len=std::hypot(dx,dy);
        if(len>1e-12) { double f=2*(len-ranges[ids[k]])/len; gx+=f*dx;gy+=f*dy; } }
      double step=0.3/result.anchors,nx=x,ny=y,next=score;
      for(unsigned attempt=0;attempt<15;++attempt) {
        nx=x-step*gx;ny=y-step*gy;
        if(result.ambiguous) { if(nx<0)nx=0;if(ny<0)ny=0; }
        next=cost(nx,ny); if(next<=score)break; step*=0.5;
      }
      if(next>score || score-next<1e-14)break;
      x=nx;y=ny;score=next;
    }
    if(score<best) { best=score;result.point={x,y,true}; }
  }
  result.rmse=std::sqrt(best/result.anchors);
  return result;
}
}
