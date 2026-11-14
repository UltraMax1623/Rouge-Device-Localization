#pragma once
#include "coordinate_engine.h"

namespace Coordinates {
struct Target { Point point, alternatives[2]; unsigned anchors=0; double rmse=0; bool ambiguous=false; };
inline Target locateTarget(const Result &layout, const double *ranges, size_t count) {
  Target result;
  size_t ids[MAX_NODES];
  if (!layout.valid || count>MAX_NODES) return result;
  for(size_t i=0;i<count;++i) if(layout.points[i].valid && std::isfinite(ranges[i]) && ranges[i]>0)
    ids[result.anchors++]=i;
  if(!result.anchors) return result;
  if (result.anchors==1) { result.ambiguous=true; return result; }
  auto cost=[&](double x,double y) {
    double total=0;
    for(unsigned k=0;k<result.anchors;++k) { const auto &a=layout.points[ids[k]];
      double e=std::hypot(x-a.x,y-a.y)-ranges[ids[k]]; total+=e*e; }
    return total;
  };
  const auto &a=layout.points[ids[0]];
  const auto &b=layout.points[ids[1]];
    double dx=b.x-a.x,dy=b.y-a.y,base=std::hypot(dx,dy);
    if(base<1e-9) { result.ambiguous=true; return result; }
    double x=(ranges[ids[0]]*ranges[ids[0]]+base*base-ranges[ids[1]]*ranges[ids[1]])/(2*base);
    double h=ranges[ids[0]]*ranges[ids[0]]-x*x;
    double y=std::sqrt(h>0?h:0);
    const double centerX=a.x+x*dx/base, centerY=a.y+x*dy/base;
    const double offsetX=-y*dy/base, offsetY=y*dx/base;
    if (result.anchors==2) {
      if(h < -1e-9) return result;
      if(y <= 1e-6) result.point={centerX,centerY,true};
      else {
        result.ambiguous=true;
        result.alternatives[0]={centerX+offsetX,centerY+offsetY,true};
        result.alternatives[1]={centerX-offsetX,centerY-offsetY,true};
      }
      return result;
    }
    bool nonCollinear=false;
    for(unsigned k=2;k<result.anchors;++k) { const auto &c=layout.points[ids[k]];
      if(std::fabs(dx*(c.y-a.y)-dy*(c.x-a.x))>1e-5*base*base) nonCollinear=true; }
    if (!nonCollinear) { result.ambiguous=true; return result; }
  double best=1e300;
  for(unsigned seed=0;seed<4;++seed) {
    const double side=seed&1 ? -1 : 1;
    double x=centerX+side*offsetX,y=centerY+side*offsetY;
    if(seed>=2) { x-=side*0.05*ranges[ids[0]]*dy/base; y+=side*0.05*ranges[ids[0]]*dx/base; }
    double score=cost(x,y);
    for(unsigned iter=0;iter<300;++iter) {
      double gx=0,gy=0;
      for(unsigned k=0;k<result.anchors;++k) { const auto &anchor=layout.points[ids[k]];
        double dx=x-anchor.x,dy=y-anchor.y,len=std::hypot(dx,dy);
        if(len>1e-12) { double f=2*(len-ranges[ids[k]])/len; gx+=f*dx;gy+=f*dy; } }
      double step=0.3/result.anchors,nx=x,ny=y,next=score;
      for(unsigned attempt=0;attempt<15;++attempt) {
        nx=x-step*gx;ny=y-step*gy;
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
