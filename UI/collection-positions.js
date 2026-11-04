"use strict";

// Proposed physical layout, not surveyed ground truth. Verify before collection.
// Preserve the T01..T21 labels from the explanatory layout image.
const outsideOffsetRatio = .25;
const diagonalOffset = outsideOffsetRatio / Math.SQRT2;
const trainingCoordinates = [
  [.25,.25],[.75,.25],[.25,.75],[.75,.75],[.5,.5],
  [.5,0],[0,.5],[1,.5],[.5,1],
  [.25,-outsideOffsetRatio],[.5,-outsideOffsetRatio],[.75,-outsideOffsetRatio],
  [.25,1+outsideOffsetRatio],[.5,1+outsideOffsetRatio],[.75,1+outsideOffsetRatio],
  [-outsideOffsetRatio,.25],[-outsideOffsetRatio,.5],[-outsideOffsetRatio,.75],
  [1+outsideOffsetRatio,.25],[1+outsideOffsetRatio,.5],[1+outsideOffsetRatio,.75],
  // At corners, equal x/y offsets are d/sqrt(2), keeping total distance d.
  [-diagonalOffset,-diagonalOffset],[1+diagonalOffset,-diagonalOffset],
  [-diagonalOffset,1+diagonalOffset],[1+diagonalOffset,1+diagonalOffset]
];
const unseenCoordinates = [
  [.25,0],[.75,0],[0,.25],[1,.25],
  [.5,.25],[.25,.5],[.75,.5],[.5,.75],
  [.125,-outsideOffsetRatio],[1+outsideOffsetRatio,.125],
  [.875,1+outsideOffsetRatio],[-outsideOffsetRatio,.875]
];
function positions(coordinates, prefix) {
  return Object.freeze(coordinates.map(([u,v],i) => Object.freeze({
    id: `${prefix}${String(i+1).padStart(2,"0")}`, u, v,
    outside: u<0 || u>1 || v<0 || v>1
  })));
}
const training = positions(trainingCoordinates,"T");
const unseen = positions(unseenCoordinates,"U");
const trainingOrder = Object.freeze([...training].sort((a,b) =>
  Math.round(((a.u-.5)**2+(a.v-.5)**2)*1e8) -
  Math.round(((b.u-.5)**2+(b.v-.5)**2)*1e8) || a.id.localeCompare(b.id)
).map(p=>p.id));
function coordinatesFor(id, sideLengthM) {
  if(![1,2,3,4,5].includes(sideLengthM)) throw new Error("Square side must be 1..5 metres.");
  const point = [...training,...unseen].find(p=>p.id===id);
  if(!point) throw new Error("Unknown target position.");
  return Object.freeze({...point, x_m:Number((point.u*sideLengthM).toFixed(6)),
    y_m:Number((point.v*sideLengthM).toFixed(6))});
}
module.exports = Object.freeze({coordinatesVerified:false,outsideOffsetRatio,training,unseen,trainingOrder,coordinatesFor});
