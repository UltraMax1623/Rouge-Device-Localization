(() => {
  const byId=id=>document.getElementById(id);
  const labels={autoscan:"Auto Scan",focus:"Focus",conf:"Node configuration",sniff:"Wi-Fi target capture",status:"Network status",calibrate:"A/n calibration",diagnose:"Measurement diagnostics",identify:"Identify board"};
  const terminal=byId("terminalOutput"), result=byId("commandResult");
  const operationMode=byId("operationMode");
  const boardSelect=byId("boardSelect");
  const identifyBoard=byId("identifyBoard");
  const defaultNodes=[1,2,3,4].map(board=>({board}));
  let online=false, locked=false, activeId=null, activeName="conf", sending=false, controlPending=false;
  let backendConnected=false, networkStatus=null;
  let resultId=null, session=null;
  const seen=new Set();
  let queuedFocus=null;
  let sendingFocus=null;
  let autoLoop=false;
  let focusedMac=null;
  let resumingAutoScan=false;
  let batchResults=[];
  let shortlist=null;
  let activeTarget=null;
  let displayedTarget=null;
  let expiryTimer;
  const dismissedCandidates=new Set();
  const sameMac=(left,right)=>Boolean(left && right &&
    left.replace(/[:-]/g,"").toUpperCase()===right.replace(/[:-]/g,"").toUpperCase());
  function showOperationMode() {
    const auto = operationMode.value === "auto";
    byId("sniffForm").hidden = auto;
    byId("autoControls").hidden = !auto;
    byId("autoScanShortlist").hidden = !auto;
  }
  function showShortlist(data) {
    if (shortlist?.commandId!==data.commandId) dismissedCandidates.clear();
    shortlist={...data,expiresAt:data.expiresAt || Date.now()+120000};
    clearTimeout(expiryTimer);
    expiryTimer=setTimeout(updateCandidateActions,Math.max(0,shortlist.expiresAt-Date.now()));
    renderShortlist();
  }
  function renderShortlist() {
    if (!shortlist) return;
    const panel=byId("autoScanShortlist");
    panel.replaceChildren(element("h2","Discovered devices"));
    if(shortlist.truncated) panel.append(element("p","Showing a limited set of sightings.","muted"));
    const candidates=shortlist.candidates.filter(candidate=>!dismissedCandidates.has(candidate.mac));
    const list=element("ul",undefined,"candidate-list");
    for(const candidate of candidates) {
      const row=element("li",undefined,"candidate-row");
      const info=element("div",undefined,"candidate-info");
      info.append(element("strong",candidate.mac),element("span",`Channel ${candidate.channel} · ${candidate.count} packets`,"muted"));
      const estimate=batchResults.find(item=>sameMac(item.mac,candidate.mac));
      if (estimate) {
        info.append(element("span",Number.isFinite(estimate.x) ? `Estimate (${estimate.x.toFixed(2)}, ${estimate.y.toFixed(2)}) m` : estimate.status,"muted"));
        if (estimate.samples) {
          info.append(element("span",`Packets at nodes 01–04: ${estimate.samples.map(count=>count ?? "—").join(" / ")}`,"muted"));
        } else if (!Number.isFinite(estimate.x) && Number.isInteger(estimate.anchors)) {
          info.append(element("span",`${estimate.anchors} receivers with usable samples`,"muted"));
        }
      }
      const focus=element("button","Focus","secondary");
      focus.type="button";
      focus.dataset.candidateMac=candidate.mac;
      focus.onclick=()=>requestFocus(candidate);
      const remove=element("button","×","candidate-remove");
      remove.type="button";
      remove.setAttribute("aria-label",`Remove ${candidate.mac} from shortlist`);
      remove.title="Remove from shortlist";
      remove.onclick=()=>{
        if (sameMac(queuedFocus?.mac,candidate.mac)) {
          fetch("/api/focus-queue",{method:"DELETE"}).catch(error=>log(error.message,"UI",true));
        }
        dismissedCandidates.add(candidate.mac);
        renderShortlist();
        const next=[...panel.querySelectorAll(".candidate-remove")][Math.min(candidates.indexOf(candidate),candidates.length-2)];
        (next || byId("autoScanButton")).focus();
      };
      row.append(info,focus,remove);
      list.append(row);
    }
    if(candidates.length) panel.append(list);
    else panel.append(element("p",shortlist.candidates.length ? "All devices removed from this list." : "No eligible devices found.","muted"));
    const expiry=element("p","Shortlist expired. Run Auto Scan to refresh it.","muted");
    expiry.id="shortlistExpiry";
    panel.append(expiry);
    updateCandidateActions();
  }
  function updateCandidateActions() {
    if (!shortlist) return;
    const expired=Date.now()>=shortlist.expiresAt;
    byId("shortlistExpiry").hidden=!expired;
    for (const button of byId("autoScanShortlist").querySelectorAll("[data-candidate-mac]")) {
      const mac=button.dataset.candidateMac;
      const inFocus=sameMac(focusedMac || activeTarget,mac) ||
        (!autoLoop && !focusedMac && !activeTarget && sameMac(displayedTarget,mac));
      const queued=sameMac(queuedFocus?.mac,mac);
      const sendingRequest=sameMac(sendingFocus,mac);
      button.textContent=queued ? "Focus queued" : sendingRequest ? "Sending Focus…" : inFocus ? "In focus" : "Focus";
      button.classList.toggle("candidate-in-focus",inFocus && !queued && !sendingRequest);
      button.disabled=expired || queued || sendingRequest || inFocus;
      button.setAttribute("aria-label",`${button.textContent}: ${mac}`);
    }
  }
  async function requestFocus(candidate) {
    if (sendingFocus) return;
    sendingFocus=candidate.mac;
    updateCandidateActions();
    try {
      const response=await fetch("/api/command",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({command:"focus",params:{mac:candidate.mac,channel:candidate.channel,scanId:shortlist.commandId}})});
      if (!response.ok) throw new Error(await response.text());
    } catch (error) {
      byId("autoScanStatus").textContent=error.message;
      log(error.message,"BACKEND",true);
    } finally {
      sendingFocus=null;
      updateCandidateActions();
    }
  }
  function element(tag,text,cls) {
    const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(cls)node.className=cls;return node;
  }
  function log(text,source="BACKEND",error=false) {
    terminal.append(element("div",`[${new Date().toLocaleTimeString()}] [${source}] ${text}`,`terminal-line ${error?"terminal-error":""}`));
    while(terminal.children.length>200)terminal.firstChild.remove();terminal.scrollTop=terminal.scrollHeight;
  }
  function controls() {
    document.querySelectorAll("[data-command]").forEach(button=>button.disabled=!online||locked||sending);
    operationMode.disabled = locked || sending;
    byId("commandAvailability").textContent=!online ? "Root unavailable. Commands will be available after reconnection." : locked ? `${labels[activeName] || activeName} is active. New commands are locked.` : "Ready for one command at a time.";
    byId("stopAutoScanButton").disabled=!(autoLoop || locked && ["autoscan","focus"].includes(activeName))||controlPending;
    byId("autoScanButton").hidden=Boolean(focusedMac);
    const resume=byId("resumeAutoScanButton");
    resume.hidden=!focusedMac;
    resume.disabled=!backendConnected || resumingAutoScan;
    const usable=locked&&online&&!controlPending;
    const calibrating=locked&&activeName==="calibrate";
    byId("calibrationReview").hidden=!calibrating;
    byId("activeCommandActions").hidden=!locked||activeName==="identify";
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
    for(const node of data?.nodes?.length ? data.nodes : defaultNodes) {
      const root=node.board===1;
      const state=current ? (node.online?"Online":"Offline") : root&&!online&&backendConnected ? "Unavailable" : "Unknown";
      const tile=element("div",undefined,"overview-node");tile.dataset.state=state.toLowerCase();
      if(node.mac)tile.title=node.mac;
      tile.append(element("strong",`${root?"Root":"Node"} ${String(node.board).padStart(2,"0")}`),element("span",state));
      nodesPanel.append(tile);
    }
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
  }
  function network(data) {
    networkStatus=data;
    window.nodeMap?.applyLayout?.(data);
    renderNetwork();
    for (const select of [boardSelect,identifyBoard]) {
      const selected=select.value;
      select.replaceChildren();
      for (const node of data.nodes||[]) {
        if (node.board===1 && select===boardSelect) continue;
        const option=element("option",`${node.board===1?"Root":"Node"} ${String(node.board).padStart(2,"0")}`);
        option.value=node.board;
        select.append(option);
      }
      if ([...select.options].some(option=>option.value===selected)) select.value=selected;
    }
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
    if(data.type==="focus_queue"){
      const previous=queuedFocus;
      const wasFocused=focusedMac;
      queuedFocus=data.selection;
      autoLoop=data.autoLoop;
      focusedMac=data.focusedMac ?? null;
      if (wasFocused && !focusedMac && autoLoop) {
        activeTarget=null;
        displayedTarget=null;
        window.nodeMap?.clearFocus?.();
        byId("autoScanStatus").textContent=locked && activeName==="focus"
          ? "Auto Scan will resume after this capture."
          : "Resuming Auto Scan when the mesh is ready.";
      }
      if (queuedFocus) byId("autoScanStatus").textContent="Focus queued for "+queuedFocus.mac;
      else if (locked && activeName==="focus" && activeTarget) byId("autoScanStatus").textContent="Focus sent for "+activeTarget;
      else if (previous) byId("autoScanStatus").textContent="Queued Focus cleared";
      updateCandidateActions();controls();return;
    }
    if(data.type==="command_lock") {
      const changed=locked!==data.locked;locked=data.locked;activeId=data.commandId;activeName=data.command||activeName;
      if (locked && ["autoscan", "focus", "sniff"].includes(activeName)) {
        operationMode.value = activeName === "sniff" ? "manual" : "auto";
        if (activeName==="focus") activeTarget=data.targetMac ?? queuedFocus?.mac ?? sendingFocus;
        showOperationMode();
      }
      if (locked && activeName === "calibrate") byId("calibrateCommand").open = true;
      if(locked){block(activeId,activeName);if(changed)log(`${labels[activeName]||activeName} started. New commands locked.`);}
      else {controlPending=false;session=null;activeTarget=null;if(changed)log("Command finished. Controls unlocked.");}
      updateCandidateActions();
      controls();renderNetwork();return;
    }
    if(data.type==="control_ack") {
      controlPending=false;log(data.message||"Action acknowledged.","ROOT",!data.accepted);controls();return;
    }
    if(data.type==="command_event") {
      const key=`${data.commandId}:${data.sequence??JSON.stringify(data)}`;
      if(seen.has(key))return;seen.add(key);if(seen.size>2048)seen.delete(seen.values().next().value);
      if(data.kind==="shortlist") {showShortlist(data);return;}
      if(data.kind==="batch") {batchResults=data.results;renderShortlist();}
      block(data.commandId,data.command);
      if(["autoscan","focus"].includes(data.command) && !queuedFocus && (data.phase==="end" || !controlPending))
        byId("autoScanStatus").textContent=data.command==="focus" && autoLoop
          ? "Resuming Auto Scan when the mesh is ready."
          : data.message || (data.phase==="end" ? labels[data.command]+" · "+data.status : labels[data.command]);
      if(["conf","sniff","autoscan","focus"].includes(data.command) && !(data.command==="focus" && autoLoop))
        window.nodeMap?.event(data);
      if (["sniff","autoscan","focus"].includes(data.command)) {
        if (data.phase==="start") displayedTarget=null;
        if (data.kind==="target" && data.command!=="sniff" && !(data.command==="focus" && autoLoop) &&
            (Number.isFinite(data.position?.x) || data.positionAlternatives?.length))
          displayedTarget=data.targetMac;
        if (data.phase==="end") activeTarget=null;
        updateCandidateActions();
      }
      if(data.phase==="start"){badge("Running");progress(`Root acknowledged ${labels[data.command]||data.command}.`);}
      else if(data.kind==="batch") {
        progress(`Estimated ${data.results.filter(item=>Number.isFinite(item.x)).length} of ${data.results.length} device positions.`);
      } else if(data.phase==="data")render(data);
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
    }catch(error){
      if (command==="focus") {
        activeTarget=null;
        byId("autoScanStatus").textContent=error.message;
        updateCandidateActions();
      }
      log(error.message,"BACKEND",true);byId("message").textContent=error.message;
    }
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
  byId("autoScanButton").onclick=()=>send("autoscan");
  byId("resumeAutoScanButton").onclick=async()=>{
    resumingAutoScan=true;controls();
    try {
      const response=await fetch("/api/auto-loop",{method:"PUT"});
      if (!response.ok) throw new Error("Could not resume Auto Scan.");
    } catch(error) {
      byId("autoScanStatus").textContent=error.message;
      log(error.message,"BACKEND",true);
    } finally {
      resumingAutoScan=false;controls();
    }
  };
  byId("stopAutoScanButton").onclick=()=>{
    byId("autoScanStatus").textContent="Stop pending";
    if (locked && activeName==="focus") action("cancel");
    else fetch("/api/auto-loop",{method:"DELETE"}).then(response=>{
      if (!response.ok) throw new Error("Could not stop Auto Scan.");
      byId("autoScanStatus").textContent=locked && activeName==="autoscan" ? "Stopping after this batch" : "Auto Scan stopped";
    })
      .catch(error=>log(error.message,"UI",true));
  };
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
  byId("identifyForm").onsubmit=event=>{
    event.preventDefault();
    send("identify",{board:Number(identifyBoard.value)});
  };
  byId("sniffForm").onsubmit = event => {
    event.preventDefault();
    send("sniff", {
      mac: byId("targetMac").value,
      channel: Number(byId("targetChannel").value)
    });
  };
  byId("calibrateButton").onclick=()=>send("calibrate",{board:Number(boardSelect.value)});
  for(const [id,name]of[["nextButton","next"],["saveButton","save"],["retryButton","retry"],["cancelButton","cancel"]])byId(id).onclick=()=>action(name);
  byId("remeasureButton").onclick=()=>action(`remeasure ${byId("remeasureDistance").value}`);
  byId("clearTerminalButton").onclick=()=>terminal.replaceChildren();
  operationMode.onchange = showOperationMode;
  showOperationMode();
  renderNetwork();
  controls();connect();
})();
