#include "../shared/coordinate_engine.h"
#include "../shared/measured_layout.h"
#include <cassert>
#include <cstdio>
int main() {
  double d[15][15] = {}; bool live[15] = {true,true,true,true};
  double x[]={0,4,4,0}, y[]={0,0,3,3};
  for(int i=0;i<4;i++) for(int j=0;j<4;j++) d[i][j]=std::hypot(x[i]-x[j],y[i]-y[j]);
  auto fit=Coordinates::solve(d,live,4);
  assert(fit.valid && fit.rmse<0.001);
  assert(fit.points[0].x==0 && fit.points[0].y==0);
  assert(fit.points[1].x>0 && fit.points[1].y==0);
  for(int i=0;i<4;i++) for(int j=i+1;j<4;j++) assert(std::fabs(std::hypot(fit.points[i].x-fit.points[j].x,fit.points[i].y-fit.points[j].y)-d[i][j])<0.003);
  fit=Coordinates::solve(d,live,2); assert(fit.valid && std::fabs(fit.points[1].x-4)<1e-6);
  live[3]=false; fit=Coordinates::solve(d,live,4); assert(fit.valid && !fit.points[3].valid);
  d[0][2]=d[2][0]=0; fit=Coordinates::solve(d,live,4); assert(!fit.valid && !fit.points[2].valid);
  d[0][2]=d[2][0]=12; fit=Coordinates::solve(d,live,3); assert(fit.valid && std::isfinite(fit.rmse));
  puts("PASS coordinate geometry, missing nodes, and inconsistent ranges");
  const auto square = Coordinates::measuredSquare(2.5);
  assert(square.valid && square.points[3].x == 2.5 && square.points[3].y == 2.5);
  assert(!Coordinates::measuredSquare(0).valid);
  const auto custom = Coordinates::measuredQuadrilateral(
      3, std::hypot(1.0,2.5), std::hypot(4.5,0.5), std::hypot(0.5,2.0), std::hypot(4.0,2.5));
  assert(custom.valid);
  assert(std::hypot(custom.points[2].x+0.5,custom.points[2].y-2) < 1e-12);
  assert(std::hypot(custom.points[3].x-4,custom.points[3].y-2.5) < 1e-12);
  assert(!Coordinates::measuredQuadrilateral(1,1,1,1,2).valid);
  assert(!Coordinates::measuredQuadrilateral(1,std::hypot(.7,.3),std::hypot(.3,.7),1,std::hypot(.3,.3)).valid);
  puts("PASS measured square, uneven sides, impossible triangle, and concave layout rejection");
}
