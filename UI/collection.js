(() => {
  const $=id=>document.getElementById(id);
  const fmt=(n,d=2)=>Number.isFinite(n)?n.toFixed(d):"—";
  function el(tag,text,cls){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(cls)e.className=cls;return e;}
  const setup = {"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T05","side_length_m":1,"target_x_m":0.5,"target_y_m":0.5,"ordinal":1,"total":25,"environment":"Clear area"};
  const windows = {"1":{"A":-60,"n":2,"receivers":[{"board":1,"mean":-42.82608696,"std":0.962432331,"count":23,"observedMs":10000},{"board":2,"mean":-48.13043478,"std":1.115283073,"count":23,"observedMs":10000},{"board":3,"mean":-58.52173913,"std":0.714420727,"count":23,"observedMs":10000},{"board":4,"mean":-57,"std":1.251086484,"count":23,"observedMs":10000}]},"2":{"A":-60,"n":2,"receivers":[{"board":1,"mean":-43.04545455,"std":0.97595048,"count":22,"observedMs":10000},{"board":2,"mean":-48.40909091,"std":1.114331879,"count":22,"observedMs":10000},{"board":3,"mean":-58.81818182,"std":1.153507049,"count":22,"observedMs":10000},{"board":4,"mean":-57.40909091,"std":1.302413526,"count":22,"observedMs":10000}]},"3":{"A":-60,"n":2,"receivers":[{"board":1,"mean":-42.43589744,"std":0.928049286,"count":78,"observedMs":10000},{"board":2,"mean":-47.70512821,"std":1.051360222,"count":78,"observedMs":10000},{"board":3,"mean":-57.96153846,"std":1.079438454,"count":78,"observedMs":10000},{"board":4,"mean":-57.11538462,"std":1.47614162,"count":78,"observedMs":10000}]}};
  const state = {setup, pending:{stage:"review", setup, windows,
    review:Object.entries(windows).map(([index,data])=>({index:Number(index), data, errors:[]}))},
    approved:[{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T05","side_length_m":1,"target_x_m":0.5,"target_y_m":0.5,"ordinal":1,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T01","side_length_m":1,"target_x_m":0.25,"target_y_m":0.25,"ordinal":2,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T02","side_length_m":1,"target_x_m":0.75,"target_y_m":0.25,"ordinal":3,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T03","side_length_m":1,"target_x_m":0.25,"target_y_m":0.75,"ordinal":4,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T04","side_length_m":1,"target_x_m":0.75,"target_y_m":0.75,"ordinal":5,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T06","side_length_m":1,"target_x_m":0.5,"target_y_m":0,"ordinal":6,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T07","side_length_m":1,"target_x_m":0,"target_y_m":0.5,"ordinal":7,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T08","side_length_m":1,"target_x_m":1,"target_y_m":0.5,"ordinal":8,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T09","side_length_m":1,"target_x_m":0.5,"target_y_m":1,"ordinal":9,"total":25,"environment":"Clear area"}},{"setup":{"mesh_id":1,"environment_id":1,"dataset_split":"train","target_id":"T11","side_length_m":1,"target_x_m":0.5,"target_y_m":-0.25,"ordinal":10,"total":25,"environment":"Clear area"}}], positions:{"training":[{"id":"T01","u":0.25,"v":0.25,"outside":false},{"id":"T02","u":0.75,"v":0.25,"outside":false},{"id":"T03","u":0.25,"v":0.75,"outside":false},{"id":"T04","u":0.75,"v":0.75,"outside":false},{"id":"T05","u":0.5,"v":0.5,"outside":false},{"id":"T06","u":0.5,"v":0,"outside":false},{"id":"T07","u":0,"v":0.5,"outside":false},{"id":"T08","u":1,"v":0.5,"outside":false},{"id":"T09","u":0.5,"v":1,"outside":false},{"id":"T10","u":0.25,"v":-0.25,"outside":true},{"id":"T11","u":0.5,"v":-0.25,"outside":true},{"id":"T12","u":0.75,"v":-0.25,"outside":true},{"id":"T13","u":0.25,"v":1.25,"outside":true},{"id":"T14","u":0.5,"v":1.25,"outside":true},{"id":"T15","u":0.75,"v":1.25,"outside":true},{"id":"T16","u":-0.25,"v":0.25,"outside":true},{"id":"T17","u":-0.25,"v":0.5,"outside":true},{"id":"T18","u":-0.25,"v":0.75,"outside":true},{"id":"T19","u":1.25,"v":0.25,"outside":true},{"id":"T20","u":1.25,"v":0.5,"outside":true},{"id":"T21","u":1.25,"v":0.75,"outside":true},{"id":"T22","u":-0.17677669529663687,"v":-0.17677669529663687,"outside":true},{"id":"T23","u":1.176776695296637,"v":-0.17677669529663687,"outside":true},{"id":"T24","u":-0.17677669529663687,"v":1.176776695296637,"outside":true},{"id":"T25","u":1.176776695296637,"v":1.176776695296637,"outside":true}],"unseen":[{"id":"U01","u":0.25,"v":0,"outside":false},{"id":"U02","u":0.75,"v":0,"outside":false},{"id":"U03","u":0,"v":0.25,"outside":false},{"id":"U04","u":1,"v":0.25,"outside":false},{"id":"U05","u":0.5,"v":0.25,"outside":false},{"id":"U06","u":0.25,"v":0.5,"outside":false},{"id":"U07","u":0.75,"v":0.5,"outside":false},{"id":"U08","u":0.5,"v":0.75,"outside":false},{"id":"U09","u":0.125,"v":-0.25,"outside":true},{"id":"U10","u":1.25,"v":0.125,"outside":true},{"id":"U11","u":0.875,"v":1.25,"outside":true},{"id":"U12","u":-0.25,"v":0.875,"outside":true}]}, savedRows:30};
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

  function tab(collection) {
    $("collectionPanel").hidden=!collection;
    $("operationsPanel").hidden=collection;
    $("collectionTab").classList.toggle("selected",collection);
    $("operationsTab").classList.toggle("selected",!collection);
    if(collection) window.nodeMap?.collection?.($("collectionNodeMap"),state);
    else window.nodeMap?.resize?.();
  }
  for(const [label,value,detail] of [
    ["MeshID",String(setup.mesh_id).padStart(2,"0"),setup.side_length_m+" m square"],
    ["TargetID",setup.target_id,"Position "+setup.ordinal+" of "+setup.total+" · centre outward"],
    ["EnvironmentID",String(setup.environment_id).padStart(2,"0"),setup.environment],
    ["Target coordinates","("+fmt(setup.target_x_m,3)+", "+fmt(setup.target_y_m,3)+") m","Physical ground truth"]
  ]) {
    const fact=el("div");
    fact.append(el("small",label),el("strong",value),el("span",detail));
    $("collectionSetup").append(fact);
  }
  $("collectionProgress").classList.add("good");
  $("collectionRequirements").textContent="Green means complete. Each receiver needed at least 20 packets in a full 10-second window.";
  $("collectionSaved").textContent=state.savedRows+" rows were saved at the time of the snapshot.";
  $("collectionTotalProgress").value=state.savedRows;
  $("confirmCollectionLayout").checked=true;
  $("confirmCollectionPlacement").checked=true;
  $("collectionTab").onclick=()=>tab(true);
  $("operationsTab").onclick=()=>tab(false);
  renderReview();
})();
