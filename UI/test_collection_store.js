"use strict";
const assert=require("node:assert/strict"),fs=require("node:fs"),os=require("node:os"),path=require("node:path");
const {CollectionStore,COLUMNS}=require("./collection-store");
const directory=fs.mkdtempSync(path.join(os.tmpdir(),"rssicollect-"));
const mac="2C:7B:A0:A9:13:61";
function window(id,index,overrides={}) {return {type:"command_event",command:"collect",commandId:id,phase:"data",kind:"collection_window",window:index,
  sequence:index,targetMac:mac,channel:6,durationMs:10000,A:-60,n:2,
  receivers:[1,2,3,4].map(board=>({board,mac:`44:33:22:11:00:0${board}`,roundId:100+index,channel:6,durationMs:10000,
    observedMs:10001,packetSpanMs:9950,status:"ok",count:100,mean:-60-board,std:1.5})),...overrides};}
try {
 let store=new CollectionStore(directory);
 assert.equal(COLUMNS.length,24);assert.equal(store.setup().target_id,"T05");
 assert.throws(()=>store.configure({mac,channel:6}),/Confirm/);
 store.configure({mac,channel:0,coordinatesConfirmed:true});
 assert.throws(()=>store.prepare("one",true),/channel/);
 store.beginDiscovery("discover-one");store.event({command:"discover",commandId:"discover-one",phase:"data",kind:"channel_discovery",channel:6});
 store=new CollectionStore(directory);store.event({command:"discover",commandId:"discover-one",phase:"data",kind:"channel_discovery",channel:6});
 store.event({command:"discover",commandId:"discover-one",phase:"end",status:"success"});
 assert.equal(store.snapshot().session.channel,6);
 store.configure({mac,channel:0,coordinatesConfirmed:true});store.beginDiscovery("cancel-discovery");
 store.event({command:"discover",commandId:"cancel-discovery",phase:"data",kind:"channel_discovery",channel:11});
 store.event({command:"discover",commandId:"cancel-discovery",phase:"end",status:"cancelled"});
 assert.equal(store.snapshot().session.channel,0);
 store.configure({mac,channel:6,coordinatesConfirmed:true});
 assert.throws(()=>store.prepare("one",false),/Confirm/);
 store.prepare("one",true);
 store.event(window("one",1));
 store=new CollectionStore(directory);assert.equal(store.snapshot().pending.stage,"collecting");
 store.event(window("one",1)); // Transport replay is idempotent.
 store.event(window("one",2));store.event(window("one",3));
 assert.throws(()=>store.approve("one")); // Needs confirmed end.
 store.event({command:"collect",commandId:"one",phase:"end",status:"success"});
 assert.equal(store.snapshot().pending.canApprove,true);
 store.approve("one");store.approve("one");assert.equal(store.snapshot().savedRows,3);
 const csv=fs.readFileSync(store.csv,"utf8").trim().split(/\r?\n/);
 assert.equal(csv.length,4);assert.equal(csv[0].split(",").length,24);
 assert(csv[1].includes('"T05"'));assert(csv[1].endsWith('"train"'));
 store=new CollectionStore(directory);assert.equal(store.snapshot().savedRows,3);
 // Load the original all-in-one journal and migrate without losing captures.
 const legacy=structuredClone(store.state);delete legacy.storageVersion;
 fs.writeFileSync(store.file,JSON.stringify(legacy));
 store=new CollectionStore(directory);
 assert.equal(store.snapshot().savedRows,3);
 assert.equal(JSON.parse(fs.readFileSync(store.file)).storageVersion,2);
 assert.deepEqual(JSON.parse(fs.readFileSync(store.file)).approved,["one"]);
 const captureFile=path.join(directory,"captures","0.json");
 const retainedCapture=fs.readFileSync(captureFile,"utf8");
 assert.notEqual(store.setup().target_id,"T05");
 store.prepare("bad",true);const bad=window("bad",1);bad.receivers[2].count=4;
 assert.equal(fs.readFileSync(captureFile,"utf8"),retainedCapture);
 assert.equal(store.snapshot(false).approved,undefined);
 store.event(bad);store.event(window("bad",2));store.event(window("bad",3));
 store.event({command:"collect",commandId:"bad",phase:"end",status:"success"});
 assert.equal(store.snapshot().pending.canApprove,false);assert.throws(()=>store.approve("bad"));
 const retained=store.snapshot().pending.windows[2];
 assert.equal(store.retry("bad","retry-one",true),1);
 store=new CollectionStore(directory);
 const retryWindow=window("retry-one",1);retryWindow.receivers.forEach(receiver=>receiver.roundId=123);
 store.event(retryWindow);store.event(retryWindow);
 store.event({command:"collect",commandId:"retry-one",phase:"end",status:"success"});
 assert.equal(store.snapshot().pending.canApprove,true);
 assert.deepEqual(store.snapshot().pending.windows[2],retained);
 assert.equal(store.snapshot().pending.windows[1].receivers[0].roundId,123);
 const previous=store.setup().target_id;store.drop("bad");assert.equal(store.setup().target_id,previous);
 for(const [name,patch] of [["duration",{durationMs:20000}],["identity",{targetMac:"00:11:22:33:44:55"}],["channel",{channel:11}]]) {
   store.prepare(name,true);store.event(window(name,1,patch));store.event(window(name,2));store.event(window(name,3));
   store.event({command:"collect",commandId:name,phase:"end",status:"success"});
   assert(!store.snapshot().pending.canApprove);store.drop(name);
 }
 store.prepare("mixed",true);const mixed=window("mixed",1);mixed.receivers[3].roundId=999;store.event(mixed);
 store.event(window("mixed",2));store.event(window("mixed",3));store.event({command:"collect",commandId:"mixed",phase:"end",status:"success"});
 assert(!store.snapshot().pending.canApprove);store.drop("mixed");
 store.prepare("cancelled",true);[1,2,3].forEach(i=>store.event(window("cancelled",i)));
 store.event({command:"collect",commandId:"cancelled",phase:"end",status:"cancelled"});assert(!store.snapshot().pending.canApprove);store.drop("cancelled");
 // The final training setup advances into a separate unseen test sequence.
 store.mutate(s=>{s.cursor={mesh:5,env:10,split:"train",position:24,complete:false};});
 store.prepare("last-train",true);[1,2,3].forEach(i=>store.event(window("last-train",i)));
 store.event({command:"collect",commandId:"last-train",phase:"end",status:"success"});store.approve("last-train");
 assert.equal(store.setup().dataset_split,"test");assert.equal(store.setup().target_id,"U05");assert.equal(store.setup().mesh_id,1);
 // CSV failure retains the approval in the authoritative journal; retry exports once.
 store.prepare("export-retry",true);[1,2,3].forEach(i=>store.event(window("export-retry",i)));
 store.event({command:"collect",commandId:"export-retry",phase:"end",status:"success"});
 const original=store.exportCsv;store.exportCsv=()=>{throw Error("file busy");};assert.throws(()=>store.approve("export-retry"),/retained/);
 assert.equal(store.snapshot().savedRows,9);assert.throws(()=>store.prepare("blocked",true),/export/);
 store.exportCsv=original;store.approve("export-retry");assert.equal(store.snapshot().savedRows,9);assert.equal(store.snapshot().exportError,null);
 assert(fs.readFileSync(store.csv,"utf8").includes('"test"'));
 console.log("PASS collection persistence, validation, approvals, deduplication, split progression, and export recovery");
} finally {fs.rmSync(directory,{recursive:true,force:true});}
module.exports={window};
