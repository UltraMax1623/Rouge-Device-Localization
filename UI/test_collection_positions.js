"use strict";
const assert=require("node:assert/strict");
const plan=require("./collection-positions");
assert.equal(plan.training.length,25);
assert.equal(plan.unseen.length,12);
assert.equal(plan.trainingOrder.length,25);
assert.equal(plan.trainingOrder[0],"T05");
const keys=new Set(plan.training.map(p=>`${p.u}:${p.v}`));
for(const p of plan.unseen) assert(!keys.has(`${p.u}:${p.v}`),"Test position overlaps a training point");
assert.equal(new Set([...plan.training,...plan.unseen].map(p=>`${p.u}:${p.v}`)).size,37);
assert.equal(plan.outsideOffsetRatio,.25);
assert.equal(plan.training.filter(p=>p.outside).length,16);
assert.equal(plan.unseen.filter(p=>p.outside).length,4);
assert.equal(plan.coordinatesFor("U01",5).x_m,1.25);
assert.equal(plan.coordinatesFor("U01",5).y_m,0);
for(const side of [1,2,3,4,5]) {
  assert.equal(plan.coordinatesFor("T05",side).x_m,side/2);
  assert.equal(plan.coordinatesFor("T05",side).y_m,side/2);
  for(const p of [...plan.training,...plan.unseen].filter(p=>p.outside)) {
    const q=plan.coordinatesFor(p.id,side);
    const dx=Math.max(0,-q.x_m,q.x_m-side);
    const dy=Math.max(0,-q.y_m,q.y_m-side);
    assert(Math.abs(Math.hypot(dx,dy)-side*.25)<1e-6,
      `${p.id} must be .25 times side length from the nearest boundary`);
  }
}
assert.throws(()=>plan.coordinatesFor("T01",0));
assert.throws(()=>plan.coordinatesFor("U99",3));
assert.equal(plan.coordinatesVerified,false);
console.log("25 training and 12 unseen positions validated; outside coordinates preserved.");
