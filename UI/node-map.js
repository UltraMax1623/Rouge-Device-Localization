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
