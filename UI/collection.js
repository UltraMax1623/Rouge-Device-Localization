(() => {
  const $=id=>document.getElementById(id);
  let state=null,online=false,busy=false,activeId=null,activeName=null,sending=false,editing=false,lastSetup=null,network=null;
  const fmt=(n,d=2)=>Number.isFinite(n)?n.toFixed(d):"—";
  function el(tag,text,cls){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(cls)e.className=cls;return e;}
  function hint(text,type=""){ $("collectionProgress").textContent=text;$("collectionProgress").className="collect-info "+type;}
  function render() {
    if(!state)return;const {session,setup,pending}=state;
    $("collectionRequirements").textContent =
      `Each receiver needs at least ${state.minimumPackets} packets in a full 10-second window. Green means complete. Orange means needs attention.`;
    const key=setup?`${setup.dataset_split}:${setup.mesh_id}:${setup.environment_id}:${setup.target_id}`:"complete";
    if(key!==lastSetup){$("confirmCollectionPlacement").checked=false;lastSetup=key;}
    if(session&&!editing){$("collectMac").value=session.mac;$("collectChannel").value=session.channel;$("confirmCollectionLayout").checked=true;}
    const locked=Boolean(session)&&!editing;
    for(const id of ["collectMac","collectChannel","confirmCollectionLayout"])$(id).disabled=locked||busy||sending||Boolean(pending);
    $("startCollectionSession").disabled=locked||busy||sending||Boolean(pending);
    $("changeCollectionSession").disabled=!session||busy||sending||Boolean(pending);
    $("fetchCollectionChannel").disabled=!locked||session.channel!==0||busy||!online||sending;
    const nodesReady=!network?.stale&&network?.nodes?.length===4&&[1,2,3,4].every(b=>network.nodes.some(n=>n.board===b&&n.online));
    $("collectConnection").textContent=!online?"Root unavailable / radio capture":!network||network.stale?"Waiting for fresh mesh status":nodesReady?"4 / 4 sniffers online":"Waiting for four configured sniffers";
    $("collectionSetup").replaceChildren();
    if(setup)for(const [name,value,sub] of [["MeshID",String(setup.mesh_id).padStart(2,"0"),`${setup.side_length_m} m square`],
      ["TargetID",setup.target_id,`Position ${setup.ordinal} of ${setup.total} · centre outward`],
      ["EnvironmentID",String(setup.environment_id).padStart(2,"0"),setup.environment],
      ["Target coordinates",`(${fmt(setup.target_x_m,3)}, ${fmt(setup.target_y_m,3)}) m`,"Read only · physical ground truth"]]) {
      const fact=el("div");fact.append(el("small",name),el("strong",value),el("span",sub));$("collectionSetup").append(fact);
    }
    $("collectSplit").textContent=setup?.dataset_split==="test"?"Unseen test data · excluded from training":"Training data";
    $("runCollection").disabled=!locked||!session.channel||!online||!nodesReady||busy||sending||Boolean(pending)||!setup||Boolean(state.exportError)||!$("confirmCollectionPlacement").checked;
    $("preflightCollection").disabled=$("runCollection").disabled;
    $("retryCollection").disabled=!locked||!online||!nodesReady||busy||sending||
      pending?.stage!=="review"||pending.canApprove||!$("confirmCollectionPlacement").checked;
    $("cancelCollection").disabled=!busy||!["collect","discover"].includes(activeName)||sending;
    $("approveCollection").disabled=busy||sending||!(pending?.canApprove||state.exportError&&state.lastApproval);
    $("approveCollection").textContent=state.exportError?"Retry CSV export":"Approve and save 3 rows";
    $("dropCollection").disabled=busy||sending||!pending||pending.stage!=="review";
    $("collectionSaved").textContent=`${state.approved.length} approved setups · ${state.savedRows} / 5,550 rows saved`;
    $("collectionFile").textContent=state.csvFile;$("collectionTotalProgress").value=state.savedRows;
    renderReview();window.nodeMap?.collection?.($("collectionNodeMap"),state);
    if(state.exportError)hint("Approval retained in the recovery journal. CSV export needs attention: "+state.exportError,"warn");
    else if(pending?.stage==="collecting") {
      const count=pending.expectedWindows??3;
      hint(online?`Capturing ${count} ${count===1?"window":"windows"}. Keep the target fixed.`:"Measuring / reconnecting. Keep the target fixed while the root retains results for delivery.","warn");
    }
    else if(pending)hint(pending.canApprove?"Three complete windows received. Review and approve to save.":`${pending.error||"Some windows still need a capture"}. Keep the setup fixed to capture only missing or failed windows.`,pending.canApprove?"good":"warn");
    else if(!setup)hint("All 25 training and 12 unseen positions are complete across five sizes and ten environments.","good");
    else if(!locked)hint("Enter the target MAC and channel, verify the layout, then lock this session.");
    else if(!session.channel)hint("Fetch the target channel once before running setups.");
    else hint(state.lastApproval?"Previous setup saved. Prepare the newly displayed position and confirm placement before Run Setup.":"Prepare the displayed position and environment, confirm placement, then Run Setup.",state.lastApproval?"good":"");
  }
  function renderReview() {
    const panel=$("collectionReview");panel.replaceChildren();const table=el("table",undefined,"collect-table"),head=el("tr");
    for(const text of ["Window","RSSI means 01 / 02 / 03 / 04","Total packets","Max SD","Review and details"])head.append(el("th",text));
    const thead=el("thead");thead.append(head);table.append(thead);const body=el("tbody");
    for(let i=1;i<=3;i++) {
      const w=state.pending?.review?.find(w=>w.index===i),data=w?.data,rows=data?.receivers||[];
      const receivers=[1,2,3,4].map(b=>rows.find(r=>r.board===b));const good=w&&w.errors.length===0;
      const spreads=receivers.map(r=>r?.std).filter(Number.isFinite);
      const row=el("tr",undefined,data?(good?"collect-good":"collect-warn"):"");
      row.append(el("td",String(i)),el("td",receivers.map(r=>fmt(r?.mean)).join(" / ")),
        el("td",data?String(receivers.reduce((s,r)=>s+(Number.isInteger(r?.count)?r.count:0),0)):"—"),
        el("td",spreads.length?fmt(Math.max(...spreads))+" dB":"—"));
      const cell=el("td"),details=el("details");details.append(el("summary",!data?(state.pending?.stage==="review"?"Missing":"Waiting"):good?"Complete":"Needs attention"));
      const list=el("ul");receivers.forEach((r,j)=>list.append(el("li",`Sniffer ${j+1}: ${fmt(r?.mean)} dBm · SD ${fmt(r?.std)} dB · ${r?.count??0} packets · observed ${fmt(r?.observedMs/1000,3)} s`)));
      if(data)for(const error of w.errors)list.append(el("li",error));details.append(list);cell.append(details);row.append(cell);body.append(row);
    }
    table.append(body);panel.append(table);
  }
  async function request(action,value={}) {
    sending=true;render();
    try {
      const response=await fetch(`/api/collection/${action}`,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(value)});
      if(!response.ok)throw Error(await response.text());state=await response.json();editing=false;render();
    }catch(error){await load();hint(error.message,"warn");}
    finally{sending=false;renderControlsOnly();}
  }
  function renderControlsOnly(){const text=$("collectionProgress").textContent,cls=$("collectionProgress").className;render();$("collectionProgress").textContent=text;$("collectionProgress").className=cls;}
  async function load(){try{const response=await fetch("/api/collection/state");if(response.ok){state=await response.json();render();}}catch{hint("Backend disconnected. Your saved data remains on the laptop.","warn");}}
  $("startCollectionSession").onclick=()=>request("session",{mac:$("collectMac").value,channel:Number($("collectChannel").value),coordinatesConfirmed:$("confirmCollectionLayout").checked});
  $("changeCollectionSession").onclick=()=>{editing=true;render();};
  $("fetchCollectionChannel").onclick=()=>request("discover");
  $("runCollection").onclick=()=>request("run",{physicalConfirmed:$("confirmCollectionPlacement").checked});
  $("preflightCollection").onclick=()=>request("preflight",{physicalConfirmed:$("confirmCollectionPlacement").checked});
  $("retryCollection").onclick=()=>request("retry",{collectionId:state.pending?.id,physicalConfirmed:$("confirmCollectionPlacement").checked});
  $("approveCollection").onclick=()=>request("approve",{collectionId:state.pending?.id||state.lastApproval});
  $("dropCollection").onclick=()=>request("drop",{collectionId:state.pending?.id});
  $("confirmCollectionPlacement").onchange=render;
  $("cancelCollection").onclick=async()=>{
    if(!activeId)return;try{const response=await fetch("/api/control",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({commandId:activeId,action:"cancel"})});
      if(!response.ok)throw Error(await response.text());hint("Cancel requested. It will reach the root after radio capture returns.","warn");
    }catch(error){hint(error.message,"warn");}
  };
  function tab(collection){$("collectionPanel").hidden=!collection;$("operationsPanel").hidden=collection;$("collectionTab").classList.toggle("selected",collection);$("operationsTab").classList.toggle("selected",!collection);
    if(collection)window.nodeMap?.collection?.($("collectionNodeMap"),state);else window.nodeMap?.resize?.();}
  $("collectionTab").onclick=()=>tab(true);$("operationsTab").onclick=()=>tab(false);
  window.addEventListener("console-message",event=>{
    const data=event.detail;
    if(data.type==="collection_state")state=data;
    if(data.type==="collection_update" && state) {
      const approved=new Map(state.approved.map(capture=>[capture.id,capture]));
      for(const capture of data.approvedAdded)approved.set(capture.id,capture);
      state={...state,...data,approved:[...approved.values()]};
    }
    if(data.type==="device_connection"){online=data.connected;if(!online)network=null;}
    if(data.type==="network_status")network=data;
    if(data.type==="command_lock"){busy=data.locked;activeId=data.commandId;activeName=data.command;}
    render();
    if(data.type==="command_event"&&["collect","discover"].includes(data.command)&&["log","error"].includes(data.phase))hint(data.message||data.code,data.phase==="error"?"warn":"");
  });
  load();
})();
