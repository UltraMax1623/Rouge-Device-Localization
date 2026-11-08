const assert=require("node:assert/strict");
const {spawn}=require("node:child_process");
const path=require("node:path");
const fs=require("node:fs"),os=require("node:os");
const {WebSocket}=require("ws");
const {parseCommand,parseControl}=require("./commands");
assert.equal(parseCommand("sniff 11:22:33:44:55:66").params.mac,"112233445566");
assert.equal(parseCommand('diagnose 2 3').params.distance,3);
assert.throws(()=>parseCommand('sniff xyz'));
assert.throws(()=>parseCommand('{"command":"calibrate","params":{"board":0}}',true));
assert.throws(()=>parseControl('{"commandId":"old","action":"cancel"}',"new","conf"));
function inbox(socket) {
  const messages=[],waiting=[];
  socket.on("message",raw=>{const value=JSON.parse(raw.toString());const index=waiting.findIndex(entry=>entry.predicate(value));
    if(index>=0){const entry=waiting.splice(index,1)[0];clearTimeout(entry.timer);entry.resolve(value);}else messages.push(value);});
  return predicate=>new Promise((resolve,reject)=>{const index=messages.findIndex(predicate);
    if(index>=0)return resolve(messages.splice(index,1)[0]);
    const entry={predicate,resolve};entry.timer=setTimeout(()=>reject(new Error("Message timeout")),8000);waiting.push(entry);
  });
}
async function connect(url, state = {}) {
  const socket=new WebSocket(url);const next=inbox(socket);
  await new Promise((resolve,reject)=>{socket.once("open",resolve);socket.once("error",reject);});
  if (url.includes("role=device")) { socket.send(JSON.stringify({type:"command_state",commandId:"",pendingEvents:false,...state})); await next(value=>value.type==="state_ack"); }
  return {socket,next};
}
(async()=>{
 const server=spawn(process.execPath,[path.join(__dirname,"server.js")],{env:{...process.env,PORT:"0",COLLECTION_DATA_DIR:fs.mkdtempSync(path.join(os.tmpdir(),"rouge-command-test-"))}});
 const sockets=[];
 try{
  const port=await new Promise((resolve,reject)=>{let out="";server.stdout.on("data",chunk=>{out+=chunk;const match=out.match(/Backend running on port (\d+)/);if(match)resolve(match[1]);});server.once("error",reject);});
  const base=`http://127.0.0.1:${port}`;
  let device=await connect(`ws://127.0.0.1:${port}/ws?role=device&token=local-device-token-change-me`);sockets.push(device.socket);
  const ui=await connect(`ws://127.0.0.1:${port}/ws?role=ui`);sockets.push(ui.socket);
  let sequence=1;
  for(const command of ["conf","sniff","status","calibrate","diagnose"]){
   const params=command==="sniff"?{mac:"11:22:33:44:55:66"}:command==="status"||command==="conf"?{}:{board:2,distance:2};
   const response=await fetch(base+"/api/command",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({command,params})});
   assert.equal(response.status,202);const accepted=await response.json();
   const request=await device.next(value=>value.type==="command");assert.equal(request.command,command);
   const emit=event=>device.socket.send(JSON.stringify({type:"command_event",commandId:accepted.commandId,command,sequence:sequence++,...event}));
   emit({phase:"start",boards:2});
   assert.equal((await ui.next(value=>value.type==="command_event"&&value.phase==="start"&&value.commandId===accepted.commandId)).command,command);
   if(["calibrate","diagnose","sniff"].includes(command)){
    const action=command==="sniff"?"cancel":"next";
    const control=await fetch(base+"/api/control",{method:"POST",body:JSON.stringify({commandId:accepted.commandId,action})});assert.equal(control.status,202);
    const received=await device.next(value=>value.type==="command_control");assert.equal(received.action,action);
    device.socket.send(JSON.stringify({type:"control_ack",commandId:accepted.commandId,controlId:received.controlId,accepted:true,message:"Action accepted"}));
    assert.equal((await ui.next(value=>value.type==="control_ack")).accepted,true);
   }
   emit({phase:"data",kind:command==="status"?"network":command,position:{x:0,y:0}});
   emit({phase:"end",status:command==="sniff"?"cancelled":"success"});
   assert.equal((await ui.next(value=>value.type==="command_event"&&value.phase==="end"&&value.commandId===accepted.commandId)).command,command);
  }
  const pendingResponse=await fetch(base+"/api/command",{method:"POST",body:"sniff 112233445566"});
  const pending=await pendingResponse.json();await device.next(value=>value.type==="command");
  const closed=new Promise(resolve=>device.socket.once("close",resolve));device.socket.close();await closed;
  await ui.next(value=>value.type==="device_connection"&&!value.connected);
  const cancel=await fetch(base+"/api/control",{method:"POST",body:JSON.stringify({commandId:pending.commandId,action:"cancel"})});
  assert.equal(cancel.status,202,"Cancel must be retained while root is off-network");
  device=await connect(`ws://127.0.0.1:${port}/ws?role=device&token=local-device-token-change-me`,{commandId:pending.commandId,command:"sniff"});sockets.push(device.socket);
  const replayedControl=await device.next(value=>value.type==="command_control");assert.equal(replayedControl.action,"cancel");
  device.socket.send(JSON.stringify({type:"control_ack",commandId:pending.commandId,controlId:replayedControl.controlId,accepted:true}));
  device.socket.send(JSON.stringify({type:"command_event",commandId:pending.commandId,command:"sniff",sequence:sequence++,phase:"end",status:"cancelled"}));
  await ui.next(value=>value.type==="command_event"&&value.commandId===pending.commandId&&value.phase==="end");
  const telemetry={type:"network_status",nodes:[{board:1,mac:"44:33:22:11:00:01",online:true}],meshChannel:11};
  device.socket.send(JSON.stringify(telemetry));
  const live=await ui.next(value=>value.type==="network_status");
  assert.equal(live.stale,false);
  assert.ok(Number.isInteger(live.receivedAt));
  const refreshed=await connect(`ws://127.0.0.1:${port}/ws?role=ui`);sockets.push(refreshed.socket);
  assert.deepEqual(await refreshed.next(value=>value.type==="network_status"),live);
  device.socket.close();
  const stale=await ui.next(value=>value.type==="network_status"&&value.stale);
  assert.equal(stale.receivedAt,live.receivedAt);
  device=await connect(`ws://127.0.0.1:${port}/ws?role=device&token=local-device-token-change-me`);sockets.push(device.socket);
  const reopened=await connect(`ws://127.0.0.1:${port}/ws?role=ui`);sockets.push(reopened.socket);
  assert.deepEqual(await reopened.next(value=>value.type==="network_status"),stale);
  device.socket.send(JSON.stringify(telemetry));
  assert.equal((await ui.next(value=>value.type==="network_status"&&!value.stale)).meshChannel,11);
  console.log("PASS command lifecycles, session controls, and telemetry freshness across reconnection");
 }finally{sockets.forEach(socket=>socket.terminate());server.kill();}
})().catch(error=>{console.error(error);process.exitCode=1;});
