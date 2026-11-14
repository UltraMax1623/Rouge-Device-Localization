#include "../shared/target_position.h"
#include <cassert>
#include <cstdio>
int main(){
 Coordinates::Result anchors;anchors.valid=true;
 anchors.points[0]={0,0,true};anchors.points[1]={4,0,true};anchors.points[2]={0,3,true};
 double d[15]={std::sqrt(5.0),std::sqrt(13.0),std::sqrt(2.0)};
 auto target=Coordinates::locateTarget(anchors,d,3);
 assert(target.point.valid&&!target.ambiguous&&target.rmse<0.001);
 assert(std::fabs(target.point.x-1)<0.003&&std::fabs(target.point.y-2)<0.003);
 target=Coordinates::locateTarget(anchors,d,2);assert(target.ambiguous&&!target.point.valid);
 assert(target.alternatives[0].valid&&target.alternatives[1].valid);
 assert(std::fabs(target.alternatives[0].y-2)<0.001&&std::fabs(target.alternatives[1].y+2)<0.001);
 target=Coordinates::locateTarget(anchors,d,1);assert(target.ambiguous&&!target.point.valid);
 d[0]=d[1]=d[2]=0;target=Coordinates::locateTarget(anchors,d,3);assert(!target.point.valid);
 d[0]=0.1;d[1]=0.1;target=Coordinates::locateTarget(anchors,d,2);assert(!target.point.valid&&!target.alternatives[0].valid);
 anchors.points[2]={7,0,true};d[0]=3;d[1]=2;d[2]=5;
 target=Coordinates::locateTarget(anchors,d,3);assert(target.ambiguous&&!target.point.valid);
 anchors.valid=false;target=Coordinates::locateTarget(anchors,d,2);assert(!target.point.valid);
 puts("PASS target positioning: one/two/three receivers, ambiguity, missing and inconsistent ranges");
}
