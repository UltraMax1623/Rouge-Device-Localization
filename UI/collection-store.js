"use strict";
const fs=require("node:fs"), path=require("node:path"), {randomUUID}=require("node:crypto");
const positions=require("./collection-positions");
const environments=["Clear area","One classroom desk","Large metal object","Several desks","Square near a wall",
  "One stationary person","Two stationary people","One walking person","Desk with books and bag","Furniture and people"];
const MIN_PACKETS=20;
const COLUMNS=["collection_id","mesh_id","target_id","environment_id","side_length_m","target_x_m","target_y_m",
  "target_mac","wifi_channel","window_index","recorded_at_utc",
  ...["rssi_mean","rssi_std","packet_count"].flatMap(name=>[1,2,3,4].map(n=>`${name}_${String(n).padStart(2,"0")}`)),"dataset_split"];
function atomicWrite(file,content) {
  const temp=file+".tmp";const fd=fs.openSync(temp,"w");
  try{fs.writeFileSync(fd,content);fs.fsyncSync(fd);}finally{fs.closeSync(fd);}
  fs.renameSync(temp,file);
}
function normalizeMac(value) {
  const hex=String(value||"").replace(/[:-]/g,"").toUpperCase();
  if(!/^[0-9A-F]{12}$/.test(hex)||hex==="000000000000"||(parseInt(hex.slice(0,2),16)&1))throw Error("Enter the target's unicast Wi-Fi MAC.");
  if(/^44332211000[1-4]$/.test(hex))throw Error("The target must be external to the four mesh boards.");
  return hex.match(/../g).join(":");
}
function validateWindow(window,session) {
  const errors=[];
  if(!window || !Array.isArray(window.receivers))return ["Window report missing"];
  if(window.durationMs!==10000||window.channel!==session.channel||normalizeSafe(window.targetMac)!==session.mac)
    errors.push("Target, channel, or duration does not match the locked session");
  if(window.receivers.length!==4)errors.push("Exactly four receiver reports are required");
  const rounds=new Set();
  for(let board=1;board<=4;board++) {
    const entries=window.receivers.filter(r=>r.board===board),r=entries[0];
    if(entries.length!==1){errors.push(`Sniffer ${board}: missing or duplicate report`);continue;}
    const expected=`44:33:22:11:00:${String(board).padStart(2,"0")}`;
    if(r.mac!==expected||r.status!=="ok")errors.push(`Sniffer ${board}: missing or failed capture`);
    if(!Number.isInteger(r.roundId)||r.roundId<=0)errors.push(`Sniffer ${board}: missing round identity`);else rounds.add(r.roundId);
    if(r.durationMs!==10000||r.channel!==session.channel||!Number.isFinite(r.observedMs)||r.observedMs<9900||r.observedMs>10100)
      errors.push(`Sniffer ${board}: incomplete 10-second window or wrong channel`);
    if(!Number.isInteger(r.count)||r.count<MIN_PACKETS||r.count>10000000)errors.push(`Sniffer ${board}: requires at least ${MIN_PACKETS} packets`);
    if(!Number.isFinite(r.mean)||r.mean< -127||r.mean>0||!Number.isFinite(r.std)||r.std<0||r.std>127)
      errors.push(`Sniffer ${board}: invalid RSSI statistics`);
  }
  if(rounds.size!==1)errors.push("Receiver reports belong to different rounds");
  return errors;
}
function normalizeSafe(mac){try{return normalizeMac(mac);}catch{return "";}}
class CollectionStore {
  constructor(directory) {
    this.directory=path.resolve(directory);fs.mkdirSync(this.directory,{recursive:true});
    this.file=path.join(this.directory,"collection-state.json");this.csv=path.join(this.directory,"collection.csv");
    this.state=fs.existsSync(this.file)?JSON.parse(fs.readFileSync(this.file,"utf8")):
      {session:null,cursor:{mesh:1,env:1,split:"train",position:0,complete:false},pending:null,approved:[],lastApproval:null};
    // The durable journal is authoritative; regenerate CSV after an interrupted export.
    this.exportError=null;try{this.exportCsv();}catch(error){this.exportError=error.message;}
  }
  persist(next) {atomicWrite(this.file,JSON.stringify(next));this.state=next;}
  mutate(fn) {const next=structuredClone(this.state);const result=fn(next);this.persist(next);return result;}
  setup() {
    const c=this.state.cursor;if(c.complete)return null;
    const id=(c.split==="train"?positions.trainingOrder:positions.unseenOrder)[c.position];
    return {mesh_id:c.mesh,environment_id:c.env,dataset_split:c.split,target_id:id,side_length_m:c.mesh,
      target_x_m:positions.coordinatesFor(id,c.mesh).x_m,target_y_m:positions.coordinatesFor(id,c.mesh).y_m,
      ordinal:c.position+1,total:c.split==="train"?25:12,environment:environments[c.env-1]};
  }
  snapshot() {
    const pending=this.state.pending?structuredClone(this.state.pending):null;
    if(pending){pending.review=[1,2,3].map(i=>({index:i,data:pending.windows[i]||null,errors:validateWindow(pending.windows[i],pending.session)}));
      pending.canApprove=pending.stage==="review"&&pending.outcome==="success"&&pending.review.every(w=>!w.errors.length);
      const ids=pending.review.map(w=>w.data?.receivers?.[0]?.roundId).filter(Number.isInteger);
      if(new Set(ids).size!==3)pending.canApprove=false;}
    return {session:this.state.session,setup:this.setup(),pending,approved:this.state.approved.map(a=>({id:a.id,setup:a.setup,mac:a.session.mac})),
      savedRows:this.state.approved.length*3,lastApproval:this.state.lastApproval,lastCapture:this.state.approved.at(-1)||null,exportError:this.exportError,
      minimumPackets:MIN_PACKETS,csvFile:this.csv,positions:{training:positions.training,unseen:positions.unseen},environments};
  }
  configure({mac,channel,coordinatesConfirmed}) {
    if(this.state.pending||this.state.discovery)throw Error("Finish the current operation before changing the session.");
    if(coordinatesConfirmed!==true)throw Error("Confirm the measured layout and corner identities before collecting.");
    if(!Number.isInteger(channel)||channel<0||channel>13)throw Error("Choose channel 1..13, or 0 to fetch it.");
    this.mutate(s=>{s.session={id:randomUUID(),mac:normalizeMac(mac),channel,coordinatesConfirmed:true};});
  }
  beginDiscovery(id) {
    if(!this.state.session||this.state.session.channel||this.state.pending||this.state.discovery)
      throw Error("Start a session with channel zero before fetching its channel.");
    this.mutate(s=>{s.discovery={id};});
  }
  prepare(commandId,physicalConfirmed) {
    if(this.exportError)throw Error("Resolve the CSV export error before capturing another setup.");
    if(this.state.pending)throw Error("Approve or drop the previous capture first.");
    if(!this.state.session?.channel)throw Error("Start a session and lock the channel first.");
    if(physicalConfirmed!==true)throw Error("Confirm the displayed target position and environment.");
    const setup=this.setup();if(!setup)throw Error("All planned setups are complete.");
    this.mutate(s=>{s.pending={id:commandId,setup,session:structuredClone(s.session),stage:"collecting",windows:{},outcome:null};});
  }
  event(message) {
    if(message.command==="discover"&&message.commandId===this.state.discovery?.id) {
      if(message.phase==="data"&&message.kind==="channel_discovery"&&Number.isInteger(message.channel)&&message.channel>=1&&message.channel<=13)
        this.mutate(s=>{s.discovery.channel=message.channel;});
      if(message.phase==="end")this.mutate(s=>{
        if(message.status==="success"&&s.discovery.channel)s.session.channel=s.discovery.channel;
        s.discovery=null;
      });
      return;
    }
    if(message.command!=="collect"||message.commandId!==this.state.pending?.id)return;
    this.mutate(s=>{
      const p=s.pending;
      if(message.phase==="data"&&message.kind==="collection_window") {
        if(!Number.isInteger(message.window)||message.window<1||message.window>3)throw Error("Invalid window index.");
        const old=p.windows[message.window];
        if(old&&JSON.stringify(old)!==JSON.stringify(message))throw Error("Conflicting report for a retained window.");
        p.windows[message.window]=message;
      }
      if(message.phase==="error")p.error=String(message.message||message.code||"Capture error");
      if(message.phase==="end"){p.stage="review";p.outcome=message.status;}
    });
  }
  approve(id) {
    const old=this.state.approved.find(a=>a.id===id);
    if(old){this.exportCsv();this.exportError=null;return;}
    const view=this.snapshot();if(view.pending?.id!==id||!view.pending.canApprove)throw Error("Three complete valid windows are required before approval.");
    this.mutate(s=>{
      const p=s.pending;
      s.approved.push({...p,recordedAt:new Date().toISOString()});s.pending=null;s.lastApproval=id;
      const c=s.cursor;c.position++;
      if(c.position===(c.split==="train"?25:12)){c.position=0;c.env++;}
      if(c.env>10){c.env=1;c.mesh++;}
      if(c.mesh>5){if(c.split==="train"){c.split="test";c.mesh=1;}else c.complete=true;}
    });
    try{this.exportCsv();this.exportError=null;}catch(error){this.exportError=error.message;throw Error("Approval retained safely, but CSV export failed. Close Excel and retry saving or downloading.");}
  }
  drop(id) {
    if(id!==this.state.pending?.id)throw Error("Capture has already changed.");
    if(this.state.pending.stage==="collecting")throw Error("Wait for capture completion or cancellation before dropping it.");
    this.mutate(s=>{s.pending=null;});
  }
  exportCsv() {
    const lines=[COLUMNS.join(",")];
    for(const capture of this.state.approved)for(let index=1;index<=3;index++) {
      const row={collection_id:capture.id,...capture.setup,target_mac:capture.session.mac,wifi_channel:capture.session.channel,
        window_index:index,recorded_at_utc:capture.recordedAt};
      for(const r of capture.windows[index].receivers){const suffix=String(r.board).padStart(2,"0");
        row[`rssi_mean_${suffix}`]=r.mean;row[`rssi_std_${suffix}`]=r.std;row[`packet_count_${suffix}`]=r.count;}
      lines.push(COLUMNS.map(k=>JSON.stringify(row[k]??"")).join(","));
    }
    atomicWrite(this.csv,lines.join("\r\n")+"\r\n");
  }
}
module.exports={CollectionStore,COLUMNS,MIN_PACKETS,validateWindow,normalizeMac};
