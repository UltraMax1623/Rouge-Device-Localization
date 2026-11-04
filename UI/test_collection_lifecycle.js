"use strict";
const assert=require("node:assert/strict"),{spawn}=require("node:child_process"),path=require("node:path"),fs=require("node:fs"),os=require("node:os"),{WebSocket}=require("ws");
const dir=fs.mkdtempSync(path.join(os.tmpdir(),"rssicollect-api-"));
const child=spawn(process.execPath,[path.join(__dirname,"server.js")],{env:{...process.env,PORT:"0",COLLECTION_DATA_DIR:dir}});
const sockets=[];
function inbox(socket) {
 const queue=[],waiting=[];socket.on("message",raw=>{const m=JSON.parse(raw);const i=waiting.findIndex(w=>w.p(m));
 if(i>=0){const w=waiting.splice(i,1)[0];clearTimeout(w.t);w.resolve(m);}else queue.push(m);});
 return p=>new Promise((resolve,reject)=>{const i=queue.findIndex(p);if(i>=0)return resolve(queue.splice(i,1)[0]);const w={p,resolve};w.t=setTimeout(()=>reject(Error("Event timeout")),6000);waiting.push(w);});
}
const deadline=setTimeout(()=>child.kill(),30000);
(async()=>{
 try {
 const port=await new Promise((resolve,reject)=>{let text="";child.stdout.on("data",c=>{text+=c;const m=text.match(/Backend running on port (\d+)/);if(m)resolve(m[1]);});child.once("exit",()=>reject(Error("Backend exited early")));});
 const base=`http://127.0.0.1:${port}`;
 async function post(action,value){return fetch(base+"/api/collection/"+action,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(value||{})});}
 assert.equal((await post("session",{mac:"2C:7B:A0:A9:13:61",channel:0,coordinatesConfirmed:true})).status,200);
 assert.equal((await post("run",{physicalConfirmed:true})).status,409); // Offline.
 const device=new WebSocket(`ws://127.0.0.1:${port}/ws?role=device&token=local-device-token-change-me`);sockets.push(device);const next=inbox(device);
 await new Promise(r=>device.once("open",r));device.send(JSON.stringify({type:"command_state",commandId:"",pendingEvents:false}));await next(m=>m.type==="state_ack");
 device.send(JSON.stringify({type:"network_status",nodes:[1,2,3,4].map(board=>({board,online:true}))}));
 const discovery=await post("discover");assert.equal(discovery.status,200);
 const discoveryCommand=await next(m=>m.type==="command");assert.equal(discoveryCommand.command,"discover");
 device.send(JSON.stringify({type:"command_event",command:"discover",commandId:discoveryCommand.commandId,sequence:10,phase:"data",kind:"channel_discovery",channel:6}));
 await next(m=>m.type==="event_ack"&&m.sequence===10);
 assert.equal(JSON.parse(fs.readFileSync(path.join(dir,"collection-state.json"))).session.channel,0,"Wait for successful discovery completion before locking");
 device.send(JSON.stringify({type:"command_event",command:"discover",commandId:discoveryCommand.commandId,sequence:11,phase:"end",status:"success"}));
 await next(m=>m.type==="event_ack"&&m.sequence===11);
 const run=await post("run",{physicalConfirmed:true});assert.equal(run.status,200);
 const state=await run.json(),id=state.pending.id,command=await next(m=>m.type==="command");
 assert.equal(command.command,"collect");assert.equal(command.params.channel,6);
 assert.equal((await post("run",{physicalConfirmed:true})).status,409);
 assert.equal((await post("approve",{collectionId:id})).status,409);
 for(let index=1;index<=3;index++) {
   const event={type:"command_event",command:"collect",commandId:id,sequence:index,phase:"data",kind:"collection_window",window:index,targetMac:"2C:7B:A0:A9:13:61",channel:6,durationMs:10000,
     receivers:[1,2,3,4].map(board=>({board,mac:`44:33:22:11:00:0${board}`,roundId:index+100,status:"ok",channel:6,durationMs:10000,observedMs:10000,count:500,mean:-60-board,std:1}))};
   device.send(JSON.stringify(event));await next(m=>m.type==="event_ack"&&m.sequence===index);
   // ACK implies its data already reached durable storage.
   assert(JSON.parse(fs.readFileSync(path.join(dir,"collection-state.json"))).pending.windows[index]);
   device.send(JSON.stringify(event));await next(m=>m.type==="event_ack"&&m.sequence===index);
 }
 device.send(JSON.stringify({type:"command_event",command:"collect",commandId:id,sequence:4,phase:"end",status:"success"}));await next(m=>m.type==="event_ack"&&m.sequence===4);
 const approved=await post("approve",{collectionId:id});assert.equal(approved.status,200);assert.equal((await approved.json()).savedRows,3);
 assert.equal((await post("approve",{collectionId:id})).status,200);
 const csv=await (await fetch(base+"/api/collection/export")).text();assert.equal(csv.trim().split(/\r?\n/).length,4);
 assert.equal((await fetch(base+"/api/command",{method:"POST",body:"collect 2C7BA0A91361 6"})).status,409);
 console.log("PASS wireless collection command, durable event ACK, concurrent-run guard, and idempotent CSV approval");
 } finally {clearTimeout(deadline);sockets.forEach(s=>s.terminate());child.kill();}
})().catch(e=>{console.error(e);process.exitCode=1;});
