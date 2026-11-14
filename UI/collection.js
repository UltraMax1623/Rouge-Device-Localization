(() => {
  const byId = id => document.getElementById(id);
  const collectionPanel = byId("collectionPanel");
  const operationsPanel = byId("operationsPanel");
  const collectionTab = byId("collectionTab");
  const operationsTab = byId("operationsTab");
  const capturePanel = byId("sampleCapture");

  const setup = {side_length_m: 1, target_id: "Sample target", dataset_split: "train"};
  const target = {id: setup.target_id, u: 0.35, v: 0.65};
  const readings = [
    [[-57, 63], [-59, 61], [-51, 72], [-57, 65]],
    [[-56, 65], [-60, 60], [-52, 71], [-58, 64]],
    [[-58, 62], [-59, 64], [-50, 75], [-57, 67]]
  ];
  const windows = Object.fromEntries(readings.map((window, index) => [index + 1, {
    A: -60,
    n: 2,
    receivers: window.map(([mean, count], receiver) => ({board: receiver + 1, mean, count}))
  }]));

  function renderMap() {
    window.nodeMap?.collection?.(byId("collectionNodeMap"), setup, target, windows);
  }

  function showTab(collection) {
    collectionPanel.hidden = !collection;
    operationsPanel.hidden = collection;
    collectionTab.classList.toggle("selected", collection);
    operationsTab.classList.toggle("selected", !collection);
    if (collection && !capturePanel.hidden) renderMap();
    if (!collection) window.nodeMap?.resize?.();
  }

  collectionTab.onclick = () => showTab(true);
  operationsTab.onclick = () => showTab(false);
  byId("showSampleCapture").onclick = () => {
    const body = byId("sampleReadings");
    body.replaceChildren();
    for (const [window, {receivers}] of Object.entries(windows)) {
      for (const receiver of receivers) {
        const row = body.insertRow();
        for (const value of [window, receiver.board, receiver.mean, receiver.count]) {
          row.insertCell().textContent = value;
        }
      }
    }
    capturePanel.hidden = false;
    renderMap();
  };
})();
