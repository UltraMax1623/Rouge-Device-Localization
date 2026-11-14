"use strict";
const fs=require("node:fs"), path=require("node:path"), {randomUUID}=require("node:crypto");
const positions=require("./collection-positions");
const environments=["Clear area","One classroom desk","Large metal object","Several desks","Square near a wall",
  "One stationary person","Two stationary people","One walking person","Desk with books and bag","Furniture and people"];
const MIN_PACKETS=5;
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
    this.captures=path.join(this.directory,"captures");
    fs.mkdirSync(this.captures,{recursive:true});
    this.state=fs.existsSync(this.file)?JSON.parse(fs.readFileSync(this.file,"utf8")):
      {session:null,cursor:{mesh:1,env:1,split:"train",position:0,complete:false},pending:null,approved:[],lastApproval:null};
    const legacy=this.state.storageVersion!==2;
    if(!legacy) this.state.approved=this.state.approved.map((id,index)=>{
      const capture=JSON.parse(fs.readFileSync(path.join(this.captures,index+".json"),"utf8"));
      if(capture.id!==id)throw Error("Capture storage identity mismatch.");
      return capture;
    });
    this.persist(this.state,legacy);
    // The durable journal is authoritative; regenerate CSV after an interrupted export.
    this.exportError=null;try{this.exportCsv();}catch(error){this.exportError=error.message;}
  }
  persist(next,forceCaptures=false) {
    const first=forceCaptures?0:this.state.approved.length;
    next.approved.slice(first).forEach((capture,offset)=>{
      const index=first+offset;
      const file=path.join(this.captures,index+".json");
      atomicWrite(file,JSON.stringify(capture));
    });
    atomicWrite(this.file,JSON.stringify({...next,storageVersion:2,approved:next.approved.map(c=>c.id)}));
    this.state=next;
  }
  mutate(fn) {
    const {approved,...current}=this.state;
    const next={...structuredClone(current),approved:approved.slice()};
    const result=fn(next);this.persist(next);return result;
  }
  setup() {
    const c=this.state.cursor;if(c.complete)return null;
    const id=(c.split==="train"?positions.trainingOrder:positions.unseenOrder)[c.position];
    return {mesh_id:c.mesh,environment_id:c.env,dataset_split:c.split,target_id:id,side_length_m:c.mesh,
      target_x_m:positions.coordinatesFor(id,c.mesh).x_m,target_y_m:positions.coordinatesFor(id,c.mesh).y_m,
      ordinal:c.position+1,total:c.split==="train"?25:12,environment:environments[c.env-1]};
  }
  snapshot(includeHistory=true) {
    const pending=this.state.pending?structuredClone(this.state.pending):null;
    if(pending){pending.review=[1,2,3].map(i=>({index:i,data:pending.windows[i]||null,errors:validateWindow(pending.windows[i],pending.session)}));
      const seen = new Set();
      for (const window of pending.review) {
        const round = window.data?.receivers?.[0]?.roundId;
        if (Number.isInteger(round) && seen.has(round)) window.errors.push("Repeated capture round");
        if (!window.errors.length) seen.add(round);
      }
      pending.canApprove=pending.stage==="review"&&["success","partial"].includes(pending.outcome)&&pending.review.every(w=>!w.errors.length);
    }
    return {session:this.state.session,setup:this.setup(),pending,
      ...(includeHistory?{approved:this.state.approved.map(a=>({id:a.id,setup:a.setup,mac:a.session.mac}))}:{}),
      savedRows:this.state.approved.length*3,lastApproval:this.state.lastApproval,lastCapture:this.state.approved.at(-1)||null,exportError:this.exportError,
      minimumPackets:MIN_PACKETS,csvFile:this.csv,
      ...(includeHistory?{positions:{training:positions.training,unseen:positions.unseen},environments}:{})};
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
  prepare(commandId,physicalConfirmed,expectedWindows=3) {
    if(this.exportError)throw Error("Resolve the CSV export error before capturing another setup.");
    if(this.state.pending)throw Error("Approve or drop the previous capture first.");
    if(!this.state.session?.channel)throw Error("Start a session and lock the channel first.");
    if(physicalConfirmed!==true)throw Error("Confirm the displayed target position and environment.");
    const setup=this.setup();if(!setup)throw Error("All planned setups are complete.");
    this.mutate(s=>{s.pending={id:commandId,setup,session:structuredClone(s.session),stage:"collecting",windows:{},outcome:null,expectedWindows,startedAt:new Date().toISOString()};});
  }
  retry(id, commandId, physicalConfirmed) {
    const pending = this.snapshot().pending;
    if (pending?.id !== id || pending.stage !== "review" || pending.canApprove)
      throw Error("Select an incomplete capture to retry.");
    if (physicalConfirmed !== true) throw Error("Confirm the target and setup stayed in place.");
    const unconfirmedAttempt = ["success","partial"].includes(pending.outcome) ? null : pending.attemptId||pending.id;
    const slots = pending.review.filter(w => w.errors.length || w.data?.commandId===unconfirmedAttempt).map(w => w.index);
    if (!slots.length) throw Error("No failed windows to retry.");
    this.mutate(s=>{
      Object.assign(s.pending,{attemptId:commandId,retrySlots:slots,expectedWindows:slots.length,stage:"collecting",outcome:null,error:null,attemptStartedAt:new Date().toISOString()});
      slots.forEach(slot => delete s.pending.windows[slot]);
    });
    return slots.length;
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
    const pending = this.state.pending;
    if(message.command!=="collect"||!pending||message.commandId!==(pending.attemptId||pending.id))return;
    if (message.phase !== "error" && message.phase !== "end" &&
        !(message.phase === "data" && message.kind === "collection_window")) return;
    let slot;
    if (message.phase === "data") {
      if (!Number.isInteger(message.window) || message.window<1 || message.window>(pending.expectedWindows??3))
        throw Error("Invalid window index.");
      slot = pending.retrySlots ? pending.retrySlots[message.window-1] : message.window;
      const old = pending.windows[slot];
      if (old) {
        const {receivedAt, capturedAt, ...original} = old;
        if (JSON.stringify(original) !== JSON.stringify(message)) throw Error("Conflicting report for a retained window.");
        return;
      }
    }
    this.mutate(s=>{
      const p=s.pending;
      if(message.phase==="data") {
        const received=Date.now();
        const start=Date.parse(p.attemptStartedAt||p.startedAt);
        const elapsed=message.captureEndElapsedMs;
        const captured=Number.isInteger(elapsed)&&elapsed>=10000 ? start+elapsed : NaN;
        p.windows[slot]={...message,receivedAt:new Date(received).toISOString()};
        if (captured<=received) p.windows[slot].capturedAt=new Date(captured).toISOString();
      }
      if(message.phase==="error")p.error=String(message.message||message.code||"Capture error");
      if(message.phase==="end"){
        p.stage="review";p.outcome=message.status;
        if(!["success","partial"].includes(message.status))
          for(const [index,window] of Object.entries(p.windows))
            if(window.commandId===message.commandId)delete p.windows[index];
      }
    });
  }
  approve(id) {
    const old=this.state.approved.find(a=>a.id===id);
    if(old){if(this.exportError){this.exportCsv();this.exportError=null;}return;}
    const view=this.snapshot();if(view.pending?.id!==id||!view.pending.canApprove)throw Error("Three complete valid windows are required before approval.");
    this.mutate(s=>{
      const p=s.pending;
      s.approved.push({...p,approvedAt:new Date().toISOString()});s.pending=null;s.lastApproval=id;
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
    const append=this.exportedCaptures!=null && fs.existsSync(this.csv) &&
      fs.statSync(this.csv).size===this.exportedBytes;
    const start=append?this.exportedCaptures:0;
    const lines=append?[]:[COLUMNS.join(",")];
    for(const capture of this.state.approved.slice(start))for(let index=1;index<=3;index++) {
      const row={collection_id:capture.id,...capture.setup,target_mac:capture.session.mac,wifi_channel:capture.session.channel,
        window_index:index,recorded_at_utc:capture.windows[index].capturedAt || capture.windows[index].receivedAt || capture.recordedAt || capture.startedAt};
      for(const r of capture.windows[index].receivers){const suffix=String(r.board).padStart(2,"0");
        row[`rssi_mean_${suffix}`]=r.mean;row[`rssi_std_${suffix}`]=r.std;row[`packet_count_${suffix}`]=r.count;}
      lines.push(COLUMNS.map(k=>JSON.stringify(row[k]??"")).join(","));
    }
    if(lines.length) {
      const content=lines.join("\r\n")+"\r\n";
      if(append) {
        const fd=fs.openSync(this.csv,"a");
        try{fs.writeFileSync(fd,content);fs.fsyncSync(fd);}
        catch(error){this.exportedCaptures=null;throw error;}
        finally{fs.closeSync(fd);}
      } else atomicWrite(this.csv,content);
    }
    this.exportedCaptures=this.state.approved.length;
    this.exportedBytes=fs.statSync(this.csv).size;
  }
}
module.exports={CollectionStore,COLUMNS,MIN_PACKETS,validateWindow,normalizeMac};
