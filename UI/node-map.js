// Cytoscape selection/inspection pattern shared with PCAP_flow/dashboard.html.
(() => {
  const status = document.getElementById("mapStatus");
  const details = document.getElementById("mapDetails");
  const reports = new Map();
  let commandId = null;
  let targetResult=null;
  const sniffReports=new Map();
  if (typeof cytoscape !== "function") {
    status.textContent = "Map library unavailable. Run npm install in UI and restart the backend.";
    window.nodeMap = { event() {} }; return;
  }
  const graph = cytoscape({
    container: document.getElementById("nodeMap"), elements: [],
    layout: { name: "preset" }, autoungrabify: true,
    minZoom: 0.02, maxZoom: 8,
    style: [
      { selector: "node", style: { label: "data(label)", "background-color": "#319cf0",
        color: "#15263d", "text-valign": "bottom", "text-margin-y": 8,
        "font-size": 12, "text-wrap": "wrap", "text-max-width": 130,
        width: 38, height: 38, "border-width": 3, "border-color": "#fff" } },
      { selector: "node.root", style: { "background-color": "#17b7a4", width: 48, height: 48 } },
      { selector: "node.target", style: { "background-color": "#ffb020", shape:"diamond", width:42,height:42 } },
      { selector: "edge.sniff", style: { "line-color":"#d8a244", "line-style":"dashed" } },
      { selector: "edge", style: { label: "data(label)", width: 2, "line-color": "#9eafc2",
        "font-size": 11, "text-background-color": "#fff", "text-background-opacity": 0.9 } },
      { selector: ":selected", style: { "border-color": "#ffb020", "line-color": "#ffb020" } }
    ]
  });
  function fitMap() { graph.fit(undefined,65); if(graph.zoom()>1.6) graph.zoom(1.6); graph.center(); }
  document.getElementById("mapFit").onclick = fitMap;
  document.getElementById("mapZoomIn").onclick = () => graph.zoom(graph.zoom()*1.3);
  document.getElementById("mapZoomOut").onclick = () => graph.zoom(graph.zoom()/1.3);
  graph.on("tap", "node", event => {
    if(event.target.hasClass("target") && targetResult) {
      details.textContent=`Target ${targetResult.targetMac}\nPosition: (${targetResult.position.x.toFixed(3)}, ${targetResult.position.y.toFixed(3)}) m\n${targetResult.message}`;
      return;
    }
    const report = reports.get(event.target.id());
    if(!report)return;
    details.textContent = `${report.reporterMac}\nPosition: (${report.position.x.toFixed(3)}, ${report.position.y.toFixed(3)}) m\nStatus: ${report.status}\n` +
      (report.distances || []).map(d => `${d.targetMac}: ${Number.isFinite(d.meters) ? d.meters.toFixed(3)+" m" : d.status}`).join("\n");
  });
  graph.on("tap", "edge", event => {
    const edge = event.target;
    if(edge.hasClass("sniff")) {
      details.textContent=`${edge.source().id()} to target ${edge.target().id()}: ${edge.data("label")} (mean across valid capture cycles)`;
      return;
    }
    const lines = [];
    for (const [a,b] of [[edge.source().id(),edge.target().id()],[edge.target().id(),edge.source().id()]]) {
      const d = reports.get(a)?.distances?.find(d => d.targetMac === b);
      lines.push(`${a} -> ${b}: ${Number.isFinite(d?.meters) ? d.meters.toFixed(3)+" m" : "unavailable"}`);
    }
    details.textContent = lines.join("\n");
  });
  function render() {
    const elements = [], placed = new Set(), unavailable = [];
    const positions=[...reports.values()].map(r=>r.position).filter(p=>Number.isFinite(p?.x)&&Number.isFinite(p?.y));
    let separation=Infinity;
    for(let i=0;i<positions.length;i++) for(let j=i+1;j<positions.length;j++) {
      const distance=Math.hypot(positions[i].x-positions[j].x,positions[i].y-positions[j].y);
      if(distance>1e-9) separation=Math.min(separation,distance);
    }
    const scale=Number.isFinite(separation) ? Math.max(80,110/separation) : 80;
    graph.scratch("metreScale",scale);
    for (const report of reports.values()) {
      const p = report.position;
      if (!Number.isFinite(p?.x) || !Number.isFinite(p?.y)) { unavailable.push(report.reporterMac); continue; }
      placed.add(report.reporterMac);
      const root = p.x === 0 && p.y === 0 && report.reporterMac.endsWith(":01");
      elements.push({ data: { id: report.reporterMac, label: `${root ? "ROOT " : ""}${report.reporterMac.slice(-2)}\n(${p.x.toFixed(2)}, ${p.y.toFixed(2)}) m` },
        classes: root ? "root" : "", position: { x: p.x*scale, y: -p.y*scale } });
    }
    const ids = [...placed];
    for (let i=0;i<ids.length;i++) for(let j=i+1;j<ids.length;j++) {
      const values = [reports.get(ids[i])?.distances?.find(d=>d.targetMac===ids[j])?.meters,
        reports.get(ids[j])?.distances?.find(d=>d.targetMac===ids[i])?.meters].filter(Number.isFinite);
      if(values.length) elements.push({ data: { id: `${ids[i]}-${ids[j]}`, source: ids[i], target: ids[j],
        label: `${(values.reduce((a,b)=>a+b,0)/values.length).toFixed(2)} m` } });
    }
    graph.elements().remove(); graph.add(elements); fitMap();
    const error = [...reports.values()].find(r=>Number.isFinite(r.position?.fitRmseMeters))?.position.fitRmseMeters;
    status.textContent = `${placed.size}/${reports.size} nodes positioned. ${placed.size===2 ? "Direction is arbitrary with two nodes. " : "Relative orientation; distances are RSSI estimates. "}` +
      (Number.isFinite(error) ? `Fit RMSE: ${error.toFixed(3)} m. ` : "") +
      (unavailable.length ? `No position: ${unavailable.join(", ")}` : "");
    details.textContent = "Select a node or connection for details. Scroll to zoom; drag the background to pan.";
  }
  window.nodeMap = {
    resize() {graph.resize();fitMap();},
    invalidateLayout() {
      reports.clear();sniffReports.clear();targetResult=null;graph.elements().remove();
      status.textContent="Calibration changed. Run Configure again to establish anchor positions.";
    },
    event(event) {
    if(event.command === "sniff") {
      if(event.phase === "start") { sniffReports.clear();targetResult=null;graph.elements(".target, .sniff").remove();status.textContent="Capturing target traffic; node positions use the last Configure result."; }
      if(event.phase === "data" && event.kind === "sniff") {
        sniffReports.set(event.reporterMac,event);
        if(Number.isFinite(event.position?.x)&&!reports.has(event.reporterMac))reports.set(event.reporterMac,{...event,distances:[]});
      }
      if(event.phase === "data" && event.kind === "target")targetResult=event;
      if(event.phase === "end") {
        if(reports.size)render();
        if(Number.isFinite(targetResult?.position?.x)&&Number.isFinite(targetResult?.position?.y)) {
          const target=targetResult.targetMac,scale=graph.scratch("metreScale")||80;
          if(graph.getElementById(target).empty())graph.add({data:{id:target,label:`TARGET\n(${targetResult.position.x.toFixed(2)}, ${targetResult.position.y.toFixed(2)}) m`},classes:"target",position:{x:targetResult.position.x*scale,y:-targetResult.position.y*scale}});
          for(const report of sniffReports.values()) {
            const value=report.distances?.[0]?.meters;
            if(Number.isFinite(value)&&!graph.getElementById(report.reporterMac).empty())graph.add({data:{id:`target-${report.reporterMac}`,source:report.reporterMac,target,label:`${value.toFixed(2)} m`},classes:"sniff"});
          }
          fitMap();status.textContent=targetResult.message;
        } else status.textContent=targetResult?.message||"No target position available. Check traffic and Configure anchor positions.";
      }
      return;
    }
    if(event.command && event.command!=="conf")return;
    if(event.phase === "start") { commandId=event.commandId; reports.clear(); status.textContent="Measuring; previous layout shown until results arrive."; }
    if(event.phase === "data") {
      if(commandId!==event.commandId) { commandId=event.commandId; reports.clear(); }
      reports.set(event.reporterMac,event);
    }
    if(event.phase === "end" && reports.size) render();
  } };
})();

// Collection uses the same Cytoscape map component with measured square anchors.
(() => {
  if(typeof cytoscape!=="function")return;
  let graph=null,context=null;
  const details=()=>document.getElementById("collectionMapDetails");
  const scale=450;
  function fit(all=false){if(!graph)return;graph.resize();graph.fit(all?graph.elements():graph.elements().not(".estimate"),55);graph.center();}
  function estimate(window,L) {
    const a=window?.A,n=window?.n;
    if(!Number.isFinite(a)||!Number.isFinite(n)||n<=0)return null;
    const r=[1,2,3,4].map(board=>window.receivers?.find(r=>r.board===board)?.mean);
    if(!r.every(Number.isFinite))return null;
    const d=r.map(rssi=>10**((a-rssi)/(10*n)));
    const x=(d[0]**2-d[1]**2+d[2]**2-d[3]**2+2*L*L)/(4*L);
    const y=(d[0]**2-d[2]**2+d[1]**2-d[3]**2+2*L*L)/(4*L);
    return Number.isFinite(x)&&Number.isFinite(y)?{x,y}:null;
  }
  window.nodeMap=window.nodeMap||{};
  window.nodeMap.collection=(container,state)=>{
    if(!state||container.offsetParent===null)return;
    if(!graph) {
      graph=cytoscape({container,layout:{name:"preset"},autoungrabify:true,minZoom:.005,maxZoom:8,style:[
        {selector:"node",style:{label:"data(label)","background-color":"#c5d1dd",width:17,height:17,color:"#415972","font-size":11,"text-valign":"bottom","text-margin-y":7,"text-wrap":"wrap","text-max-width":125,"border-width":2,"border-color":"white"}},
        {selector:"node.anchor",style:{shape:"round-rectangle","background-color":"#078a95",width:32,height:32,color:"#155666","font-weight":"bold"}},
        {selector:"node.saved",style:{"background-color":"#179b66"}},
        {selector:"node.test",style:{shape:"diamond","border-color":"#326dd5"}},
        {selector:"node.actual",style:{shape:"diamond","background-color":"#ef9e1b",width:28,height:28,"border-color":"#bb740c","font-weight":"bold"}},
        {selector:"node.estimate",style:{shape:"star","background-color":"#357ee6",width:27,height:27,"font-weight":"bold","z-index":10}},
        {selector:"edge",style:{width:2,"line-color":"#9aafc0"}},
        {selector:":selected",style:{"border-color":"#6737c1","border-width":4}}
      ]});
      graph.on("tap","node",async event=>{
        const data=event.target.data();details().textContent=data.description||data.label;
        if(data.captureId)try {
          const response=await fetch(`/api/collection/capture?id=${encodeURIComponent(data.captureId)}`);
          if(!response.ok)return;const saved=await response.json();
          details().textContent=data.description+"\n"+[1,2,3].map(index=>`W${index}: `+saved.windows[index].receivers.map(r=>`${r.board}: ${r.mean.toFixed(2)} dBm / SD ${r.std.toFixed(2)} / ${r.count} pkts`).join("; ")).join("\n");
        }catch{}
      });
      document.getElementById("collectionMapFit").onclick=()=>fit();
      document.getElementById("collectionMapEstimates").onclick=()=>fit(true);
    }
    const setup=state.setup||state.lastCapture?.setup;if(!setup)return;
    const L=setup.side_length_m;
    const key=`${setup.mesh_id}:${setup.environment_id}:${setup.dataset_split}:${state.session?.mac}:${state.pending?.id}:${state.pending?.stage}:${Object.keys(state.pending?.windows||{}).length}:${state.savedRows}`;
    if(key===context){graph.resize();return;}context=key;
    const elements=[];
    [[0,0],[1,0],[0,1],[1,1]].forEach(([u,v],i)=>elements.push({data:{id:`anchor-${i+1}`,label:`Sniffer 0${i+1}`,description:`Sniffer 0${i+1}: (${(u*L).toFixed(3)}, ${(v*L).toFixed(3)}) m · measured corner`},position:{x:u*scale,y:v*scale},classes:"anchor"}));
    for(const [i,j] of [[1,2],[2,4],[4,3],[3,1]])elements.push({data:{id:`side-${i}-${j}`,source:`anchor-${i}`,target:`anchor-${j}`}});
    const points=setup.dataset_split==="test"?state.positions.unseen:state.positions.training;
    for(const p of points) {
      const saved=state.approved.find(a=>a.setup.mesh_id===setup.mesh_id&&a.setup.environment_id===setup.environment_id&&a.setup.target_id===p.id&&a.mac===state.session?.mac);
      const current=p.id===setup.target_id;
      elements.push({data:{id:p.id,label:current?`${p.id} · actual`:p.id,captureId:saved?.id,
        description:`${p.id}: (${(p.u*L).toFixed(3)}, ${(p.v*L).toFixed(3)}) m · ${saved?"approved":"pending"} · ${setup.dataset_split}`},
        position:{x:p.u*scale,y:p.v*scale},classes:`${saved?"saved ":""}${current?"actual ":""}${setup.dataset_split==="test"?"test":""}`});
    }
    const capture=state.pending||state.lastCapture;
    const estimateList=document.getElementById("collectionWindowEstimates");estimateList.replaceChildren();
    let count=0;
    if(capture&&capture.setup.mesh_id===setup.mesh_id&&capture.setup.environment_id===setup.environment_id&&capture.setup.dataset_split===setup.dataset_split) {
      for(let index=1;index<=3;index++) {
        const w=capture.windows[index],check=capture.review?.find(r=>r.index===index);
        if(!w||check?.errors.length)continue;const p=estimate(w,L);if(!p)continue;count++;
        const button=document.createElement("button");button.type="button";
        button.textContent=`W${index} · ${capture.setup.target_id}: (${p.x.toFixed(3)}, ${p.y.toFixed(3)}) m · baseline estimate`;
        const description=`Window ${index} baseline estimate: (${p.x.toFixed(3)}, ${p.y.toFixed(3)}) m\nActual ${capture.setup.target_id}: (${capture.setup.target_x_m}, ${capture.setup.target_y_m}) m\nRSSI A/n baseline estimate; not a trained ML prediction.`;
        button.onclick=()=>{details().textContent=description;};estimateList.append(button);
        const overlapping=elements.find(e=>e.classes==="estimate"&&Math.hypot(e.position.x-p.x/L*scale,e.position.y-p.y/L*scale)<1e-7);
        if(overlapping){overlapping.data.label+=` / W${index}`;overlapping.data.description+="\n"+description;continue;}
        elements.push({data:{id:`estimate-${index}`,label:`W${index} · ${capture.setup.target_id}`,
          description:`Window ${index} baseline estimate: (${p.x.toFixed(3)}, ${p.y.toFixed(3)}) m\nActual ${capture.setup.target_id}: (${capture.setup.target_x_m}, ${capture.setup.target_y_m}) m\nRSSI A/n baseline estimate; not a trained ML prediction. Overlapping markers retain their real coordinates.`},
          position:{x:p.x/L*scale,y:p.y/L*scale},classes:"estimate"});
      }
    }
    graph.elements().remove();graph.add(elements);fit();
    document.getElementById("collectionMapStatus").textContent=`Measured ${L} m square · y increases downward · ${count}/3 per-window baseline estimates available. Fit all estimates to include estimates beyond this view. Overlapping estimates share coordinates.`;
    if(count)details().textContent="Select W1, W2 or W3 for estimated coordinates. Select a saved position for its three recorded windows.";
  };
})();
