(() => {
  const byId=id=>document.getElementById(id);
  const labels={conf:"Node configuration",sniff:"Wi-Fi target capture",status:"Network status",calibrate:"A/n calibration",diagnose:"Measurement diagnostics"};
  const terminal=byId("terminalOutput"), result=byId("commandResult");
  let online=false, locked=false, activeId=null, activeName="conf", sending=false, controlPending=false;
  let backendConnected=false, networkStatus=null;
  let resultId=null, session=null;
  const seen=new Set();
  function element(tag,text,cls) {
    const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(cls)node.className=cls;return node;
  }
  function log(text,source="BACKEND",error=false) {
    terminal.append(element("div",`[${new Date().toLocaleTimeString()}] [${source}] ${text}`,`terminal-line ${error?"terminal-error":""}`));
    while(terminal.children.length>200)terminal.firstChild.remove();terminal.scrollTop=terminal.scrollHeight;
  }
  function controls() {
    document.querySelectorAll("[data-command]").forEach(button=>button.disabled=!online||locked||sending);
    byId("commandAvailability").textContent=!online ? "Root unavailable. Commands will be available after reconnection." : locked ? `${labels[activeName] || activeName} is active. New commands are locked.` : "Ready for one command at a time.";
    const usable=locked&&online&&!controlPending;
    byId("nextButton").disabled=!usable||!session?.canNext;
    byId("saveButton").disabled=!usable||!session?.canSave;
    byId("retryButton").disabled=!usable||!session?.canReview;
    byId("remeasureButton").disabled=!usable||!session?.canReview;
    byId("cancelButton").disabled=!locked||controlPending||session?.state==="saving";
    byId("sessionHint").textContent=controlPending ? "Action sent; waiting for the root. It may be temporarily capturing traffic." : session?.message || "Placement and review actions appear during a session.";
  }
  function block(id,name=activeName) {
    if(resultId===id)return result;
    resultId=id;session=null;
    result.replaceChildren();
    const heading=element("div",undefined,"result-heading");
    heading.append(element("strong",labels[name]||name),element("span","Waiting","result-badge"));
    result.append(heading,element("p","Waiting for the root to acknowledge.","result-progress"),element("div",undefined,"result-nodes"));
    return result;
  }
  function progress(text) { result.querySelector(".result-progress").textContent=text; }
  function badge(text) {
    const badge=result.querySelector(".result-heading .result-badge");
    badge.textContent=text;badge.className="result-badge "+(["success","failed","interrupted","cancelled","partial"].includes(text)?text:"");
  }
  function table(headers,rows) {
    const table=element("table",undefined,"result-table"),head=element("thead"),line=element("tr"),body=element("tbody");
    headers.forEach(text=>line.append(element("th",text)));head.append(line);
    rows.forEach(values=>{const row=element("tr");values.forEach(value=>row.append(element("td",String(value))));body.append(row);});
    table.append(head,body);return table;
  }
  const number=(value,digits=3)=>Number.isFinite(value)?value.toFixed(digits):"Unavailable";
  function positionText(p) { return Number.isFinite(p?.x)&&Number.isFinite(p?.y)?`(${number(p.x)}, ${number(p.y)}) m`:"Unavailable"; }
  function card(key,title,subtitle,status) {
    const panel=result.querySelector(".result-nodes");
    const old=[...panel.children].find(node=>node.dataset.key===key);if(old)old.remove();
    const card=element("div",undefined,"result-node");card.dataset.key=key;
    const head=element("div",undefined,"result-node-head"),name=element("div");
    name.append(element("strong",title),element("span",subtitle,"result-mac"));
    head.append(name,element("span",status,"result-badge "+(status==="ok"?"success":"")));
    card.append(head);panel.append(card);return card;
  }
  function renderNetwork() {
    const data=networkStatus;
    const current=backendConnected&&online&&data&&!data.stale;
    const connection=byId("connectionStatus");
    connection.textContent=!backendConnected ? "Dashboard disconnected from backend"
      : current ? "Root connected to backend"
      : online ? "Root connected to backend · waiting for fresh mesh status"
      : locked ? "Root unavailable · operation in progress" : "Root unavailable";
    const factsPanel=byId("networkFacts"), nodesPanel=byId("overviewNodes");
    factsPanel.replaceChildren();
    nodesPanel.replaceChildren();
    if(!data) {
      factsPanel.append(element("span","Waiting for root telemetry"));
      return;
    }
    const lastUpdate=new Date(data.receivedAt).toLocaleTimeString();
    factsPanel.append(element("span",current ? `Updated ${lastUpdate}` : `Last known at ${lastUpdate} · awaiting fresh telemetry`,current?"":"network-stale"));
    const facts=[`Hotspot: ${data.ssid||"not associated"}`,`Root IP: ${data.ip||"unknown"}`,`Server: ${data.serverIP}:${data.serverPort}`,
      `Channels: mesh ${data.meshChannel} / hotspot ${data.routerChannel||"unknown"}`,`A: ${number(data.A,2)} / n: ${number(data.n,2)}`,
      `Model: ${data.modelSynced?"synchronized":"sync pending"}`,`Mode: ${data.activeCommand||data.mode||"idle"}`,`Wi-Fi: ${data.rssi} dBm`,
      `Uptime: ${data.uptimeSeconds}s`];
    facts.forEach(text=>factsPanel.append(element("span",text)));
    for(const node of data.nodes||[]) {
      const root=node.board===1;
      const state=current ? (node.online?"Online":"Offline") : root&&!online ? "Unavailable" : "Unknown";
      const tile=element("div",undefined,"overview-node");tile.dataset.state=state.toLowerCase();
      tile.append(element("strong",`${root?"Root":"Node"} ${String(node.board).padStart(2,"0")} · ${state}`),element("span",node.mac));
      nodesPanel.append(tile);
    }
  }
  function network(data) {
    networkStatus=data;
    window.nodeMap?.applyLayout?.(data);
    renderNetwork();
    const boardSelects=[byId("boardSelect"),byId("diagnosticBoardSelect")];
    const selected=boardSelects.map(select=>select.value);
    boardSelects.forEach(select=>select.replaceChildren());
    for(const node of data.nodes||[]) {
      if(node.board!==1)for(const select of boardSelects){const option=element("option",`Node ${String(node.board).padStart(2,"0")}`);option.value=node.board;select.append(option);}
    }
    boardSelects.forEach((select,index)=>{if([...select.options].some(option=>option.value===selected[index]))select.value=selected[index];});
  }
  function render(data) {
    if(data.kind==="network") {
      const report=card("network","Connection and model",data.rootMac,"ok");
      report.append(table(["PROPERTY","VALUE"],[
        ["Root IP",data.ip],["Backend",`${data.serverIP}:${data.serverPort}`],["Mesh / hotspot channel",`${data.meshChannel} / ${data.routerChannel}`],
        ["A / n",`${number(data.A)} / ${number(data.n)}`],["Model",data.modelSynced?"Synchronized":"Sync pending"],
        ["Free memory",`${data.freeHeap} bytes`]
      ]),table(["BOARD","MAC","STATUS AT REQUEST"],(data.nodes||[]).map(node=>[
        String(node.board).padStart(2,"0"),node.mac,node.online?"Online":"Offline"
      ])));return;
    }
    if(data.kind==="calibration"||data.kind==="diagnostic") {
      session=data;progress(data.message||"Processing");badge(data.state);
      const item=card("session",data.kind==="diagnostic"?"Fixed-distance windows":"Calibration readings",`Root 01 and node ${String(data.board).padStart(2,"0")}`,data.state);
      item.append(element("div",`Placement: ${number(data.distanceMeters,2)} m · Attempt ${data.attempt} · Current A/n: ${number(data.A,2)} / ${number(data.n,2)}`,"result-position"));
      if(Number.isFinite(data.candidateA))item.append(element("div",`Candidate A/n: ${number(data.candidateA,2)} / ${number(data.candidateN,2)} · Fit RMSE: ${number(data.fitRmseDb,2)} dB`,"result-position"));
      if(data.validation)item.append(element("div",`Validation at 2.5 m: ${data.validation.passed?"accepted":"rejected"} · Fitted error ${number(data.validation.meanErrorMeters)} m / previous error ${number(data.validation.previousErrorMeters)} m`,"result-position"));
      item.append(table([data.kind==="diagnostic"?"WINDOW":"DISTANCE","ROOT RSSI","NODE RSSI"],(data.readings||[]).map(row=>[
        data.kind==="diagnostic"?`${row.window} · ${row.distanceMeters} m`:`${row.distanceMeters} m`,
        `${number(row.rootRssi,2)}${row.rootValid===false?" (flagged)":""}`,`${number(row.nodeRssi,2)}${row.nodeValid===false?" (flagged)":""}`
      ])));
      if(data.kind==="calibration" && data.readings?.some(row=>Number.isFinite(row.rootEstimatedMeters)))
        item.append(table(["ACTUAL","ROOT ESTIMATE","NODE ESTIMATE","CHECK"],data.readings.map(row=>[
          `${row.distanceMeters} m`,number(row.rootEstimatedMeters,2)+" m",number(row.nodeEstimatedMeters,2)+" m",row.recheck?"Remeasure":"Within tolerance"
        ])));
      if(data.kind==="diagnostic" && data.radioChecks) {
        const details=element("details");details.append(element("summary","Samples, spread, and radio settings"));
        details.append(table(["WINDOW","ROOT SAMPLES / SPREAD","NODE SAMPLES / SPREAD"],Object.entries(data.radioChecks).map(([window,row])=>[
          window,`${row.rootSamples??0} / ${number(row.rootSpreadDb,2)} dB`,`${row.nodeSamples??0} / ${number(row.nodeSpreadDb,2)} dB`
        ])));
        const radio=Object.values(data.radioChecks).at(-1);
        if(radio)for(const [receiver,settings]of[["Root",radio.rootRadio],["Node",radio.nodeRadio]])
          if(settings)details.append(table([receiver+" RADIO","VALUE"],Object.entries(settings).map(([key,value])=>[key,typeof value==="object"?JSON.stringify(value):String(value)])));
        item.append(details);
      }
      controls();return;
    }
    if(data.kind==="target") {
      const item=card("target","Target position",data.targetMac,data.position?.status||"unavailable");
      item.append(element("div",`Position: ${positionText(data.position)} · Receivers: ${data.anchors} · Fit RMSE: ${number(data.position?.fitRmseMeters)} m`,"result-position"));
      item.append(element("p",data.message,"result-progress"));
      if(data.positionAlternatives?.length)item.append(table(["POSSIBLE POSITION"],data.positionAlternatives.map(p=>[positionText(p)])));
      return;
    }
    if(data.reporterMac) {
      const title=data.reporterMac.endsWith(":01")?"Root 01":`Node ${data.reporterMac.slice(-2)}`;
      const item=card(data.reporterMac,title,data.reporterMac,data.status);
      const positionLabel = data.position?.status === "measured" ? "Measured position" : "Estimated position";
      item.append(element("div",`${positionLabel}: ${positionText(data.position)}${data.kind==="sniff"?` · Valid windows ${data.windowNumbers?.join(", ")||"none"} (${data.validCycles}/${data.cycles}) · Channel ${data.channel}`:""}`,"result-position"));
      if (data.distances?.length) item.append(table(["MEASURED TARGET","DISTANCE"],data.distances.map(row=>[row.targetMac,Number.isFinite(row.meters)?number(row.meters,2)+" m":row.status||"Unavailable"])));
    }
  }
  function receive(data) {
    window.dispatchEvent(new CustomEvent("console-message",{detail:data}));
    if(data.type==="network_status"){network(data);return;}
    if(data.type==="command_lock") {
      const changed=locked!==data.locked;locked=data.locked;activeId=data.commandId;activeName=data.command||activeName;
      if(locked){block(activeId,activeName);if(changed)log(`${labels[activeName]||activeName} started. New commands locked.`);}
      else {controlPending=false;session=null;if(changed)log("Command finished. Controls unlocked.");}
      controls();renderNetwork();return;
    }
    if(data.type==="control_ack") {
      controlPending=false;log(data.message||"Action acknowledged.","ROOT",!data.accepted);controls();return;
    }
    if(data.type==="command_event") {
      const key=`${data.commandId}:${data.sequence??JSON.stringify(data)}`;
      if(seen.has(key))return;seen.add(key);if(seen.size>2048)seen.delete(seen.values().next().value);
      block(data.commandId,data.command);
      if(["conf","sniff"].includes(data.command))window.nodeMap?.event(data);
      if(data.phase==="start"){badge("Running");progress(`Root acknowledged ${labels[data.command]||data.command}.`);}
      else if(data.phase==="data")render(data);
      else if(data.phase==="log")progress(data.message||"Processing...");
      else if(data.phase==="error")result.append(element("div",data.message||data.code,"result-alert"));
      else if(data.phase==="end") {
        badge(data.status);progress(data.message||`Command ${data.status}. Review the results below.`);
        byId("message").textContent=`${labels[data.command]||data.command}: ${data.status}`;
      }
      return;
    }
    if(data.type==="device_connection") {
      online=data.connected;
      if(!online&&networkStatus)networkStatus={...networkStatus,stale:true};
      renderNetwork();
      log(`Root ${online?"connected":"disconnected"}.`);controls();return;
    }
    if(data.type==="log")log(data.message||"",data.source||"ROOT");
  }
  function connect() {
    const socket=new WebSocket(`${location.protocol==="https:"?"wss:":"ws:"}//${location.host}/ws?role=ui`);
    socket.onmessage=event=>{try{receive(JSON.parse(event.data));}catch(error){log(`Dashboard could not display an update: ${error.message}`,"UI",true);}};
    socket.onopen=()=>{backendConnected=true;renderNetwork();log("Dashboard connected.");};
    socket.onclose=()=>{
      backendConnected=false;online=false;
      if(networkStatus)networkStatus={...networkStatus,stale:true};
      window.dispatchEvent(new CustomEvent("console-message",{detail:{type:"device_connection",connected:false}}));
      controls();renderNetwork();setTimeout(connect,2000);
    };
    socket.onerror=()=>log("Backend WebSocket error.","UI",true);
  }
  async function send(command,params={}) {
    sending=true;controls();
    try {
      const response=await fetch("/api/command",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({command,params})});
      const text=await response.text();if(!response.ok)throw new Error(text);
      const accepted=JSON.parse(text);block(accepted.commandId,command);
    }catch(error){log(error.message,"BACKEND",true);byId("message").textContent=error.message;}
    finally{sending=false;controls();}
  }
  async function action(action) {
    if(!activeId)return;controlPending=true;controls();
    try {
      const response=await fetch("/api/control",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({commandId:activeId,action})});
      if(!response.ok)throw new Error(await response.text());
      log(`${action} requested. Waiting for root acknowledgement.`,"UI");
    }catch(error){controlPending=false;log(error.message,"BACKEND",true);controls();}
  }
  const layoutMode = byId("layoutMode");
  function showLayoutFields() {
    for (const [id, mode] of [["squareLayoutFields","square"],["manualLayoutFields","manual"]]) {
      const fields = byId(id);
      fields.hidden = fields.disabled = layoutMode.value !== mode;
    }
  }
  layoutMode.onchange = showLayoutFields;
  showLayoutFields();
  byId("configureForm").onsubmit = event => {
    event.preventDefault();
    if (layoutMode.value === "square") {
      send("conf", {layout: {mode: "square", side: Number(byId("squareSide").value)}});
    } else if (layoutMode.value === "manual") {
      const lengths = ["side0102","side0204","side0403","side0301","diagonal0104"].map(id => Number(byId(id).value));
      send("conf", {layout: {mode: "manual", lengths}});
    } else {
      send("conf");
    }
  };
  byId("statusButton").onclick=()=>send("status");
  byId("sniffForm").onsubmit = event => {
    event.preventDefault();
    send("sniff", {
      mac: byId("targetMac").value,
      channel: Number(byId("targetChannel").value)
    });
  };
  byId("calibrateButton").onclick=()=>send("calibrate",{board:Number(byId("boardSelect").value)});
  byId("diagnoseButton").onclick=()=>send("diagnose",{board:Number(byId("diagnosticBoardSelect").value),distance:Number(byId("diagnosticDistance").value)});
  for(const [id,name]of[["nextButton","next"],["saveButton","save"],["retryButton","retry"],["cancelButton","cancel"]])byId(id).onclick=()=>action(name);
  byId("remeasureButton").onclick=()=>action(`remeasure ${byId("remeasureDistance").value}`);
  byId("clearTerminalButton").onclick=()=>terminal.replaceChildren();
  controls();connect();
})();
