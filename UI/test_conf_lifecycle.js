const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const path = require("node:path");
const fs = require("node:fs"), os = require("node:os");
const { WebSocket } = require("ws");

async function run() {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),"rouge-conf-test-"));
  const server = spawn(process.execPath, [path.join(__dirname, "server.js")], {
    cwd: __dirname,
    env: { ...process.env, PORT: "0", COMMAND_TIMEOUT_MS: "500", COLLECTION_DATA_DIR:directory },
    stdio: ["ignore", "pipe", "pipe"]
  });
  const sockets = [];
  const timer = setTimeout(() => { server.kill(); }, 10000);
  try {
    const port = await new Promise((resolve, reject) => {
      let output = "";
      server.stdout.on("data", chunk => {
        output += chunk.toString();
        const match = output.match(/Backend running on port (\d+)\./);
        if (match) resolve(Number(match[1]));
      });
      server.once("error", reject);
      server.once("exit", code => reject(new Error(`Backend exited: ${code}`)));
    });
    const connect = url => new Promise((resolve, reject) => {
      const socket = new WebSocket(url);
      sockets.push(socket);
      socket.once("open", () => resolve(socket));
      socket.once("error", reject);
    });
    const ui = await connect(`ws://127.0.0.1:${port}/ws?role=ui`);
    const device = await connect(`ws://127.0.0.1:${port}/ws?role=device`);
    const forbidden = await fetch(`http://127.0.0.1:${port}/`, {headers:{Origin:"http://elsewhere.invalid"}});
    assert.equal(forbidden.status,403);
    const stateAcknowledged = new Promise(resolve => device.once("message", resolve));
    device.send(JSON.stringify({ type: "command_state", commandId: "", pendingEvents: false }));
    await stateAcknowledged;
    const events = [];
    const locks = [];
    ui.on("message", raw => { const state = JSON.parse(raw.toString()); if (state.type === "command_lock") locks.push(state.locked); });
    const acknowledgements = [];
    device.on("message", raw => { const msg = JSON.parse(raw.toString()); if (msg.type === "event_ack") acknowledgements.push(msg); });
    const ended = new Promise(resolve => ui.on("message", raw => {
      const event = JSON.parse(raw.toString());
      if (event.type === "command_event") {
        events.push(event);
        if (event.phase === "end") resolve();
      }
    }));
    const command = new Promise(resolve => device.once("message", raw => resolve(JSON.parse(raw.toString()))));
    const response = await fetch(`http://127.0.0.1:${port}/api/command`, {
      method: "POST", body: "conf"
    });
    assert.equal(response.status, 202);
    const accepted = await response.json();
    const request = await command;
    assert.equal(request.commandId, accepted.commandId);
    assert.equal(request.command, "conf");
    const duplicate = await fetch(`http://127.0.0.1:${port}/api/command`, {
      method: "POST", body: "conf"
    });
    assert.equal(duplicate.status, 409);
    let sequence = 0;
    for (const event of [
      { phase: "start", status: "accepted", boards: 2 },
      { phase: "data", reporterMac: "44:33:22:11:00:01", status: "ok",
        distances: [{ targetMac: "44:33:22:11:00:02", meters: 2.4, status: "ok" }] },
      { phase: "end", status: "success" }
    ]) device.send(JSON.stringify({ type: "command_event", commandId: accepted.commandId,
      command: "conf", sequence: ++sequence, ...event }));
    await ended;
    assert.deepEqual(events.map(event => event.phase), ["start", "data", "end"]);
    assert.equal(events[1].distances[0].meters, 2.4);
    device.send(JSON.stringify(events[1]));
    await new Promise(resolve => setTimeout(resolve, 50));
    assert.equal(events.length, 3, "replayed event must not reach UI twice");
    assert.ok(locks.includes(true), "backend must publish a locked state");
    assert.equal(locks.at(-1), false, "backend must unlock after end");
    assert.ok(acknowledgements.length >= 4, "backend acknowledges original and replayed events");
    const rejectedEvents = [];
    const rejected = new Promise(resolve => ui.on("message", raw => {
      const event = JSON.parse(raw.toString());
      if (event.type === "command_event" && event.commandId !== accepted.commandId) {
        rejectedEvents.push(event);
        if (event.phase === "end") resolve();
      }
    }));
    const secondCommand = new Promise(resolve => device.once("message", raw => resolve(JSON.parse(raw.toString()))));
    const secondResponse = await fetch(`http://127.0.0.1:${port}/api/command`, {
      method: "POST", body: "conf"
    });
    assert.equal(secondResponse.status, 202);
    const second = await secondCommand;
    device.send(JSON.stringify({ type: "command_event", commandId: second.commandId,
      command: "conf", phase: "error", code: "MESH_NOT_READY", message: "Peer missing" }));
    device.send(JSON.stringify({ type: "command_event", commandId: second.commandId,
      command: "conf", phase: "end", status: "failed" }));
    await rejected;
    assert.deepEqual(rejectedEvents.map(event => event.phase), ["error", "end"]);
    const unknown = new Promise(resolve => ui.on("message", raw => {
      const event = JSON.parse(raw.toString());
      if (event.code === "OUTCOME_UNKNOWN") resolve(event);
    }));
    const thirdCommand = new Promise(resolve => device.once("message", raw => resolve(JSON.parse(raw.toString()))));
    await fetch(`http://127.0.0.1:${port}/api/command`, { method: "POST", body: "conf" });
    const third = await thirdCommand;
    const stateRequested=new Promise(resolve=>device.once("message",raw=>resolve(JSON.parse(raw))));
    const timedOut = await unknown;
    assert.equal(timedOut.commandId, third.commandId);
    assert.equal((await stateRequested).type,"command_state_request");
    const recovered = new Promise(resolve => ui.on("message", raw => {
      const event = JSON.parse(raw.toString());
      if (event.commandId === third.commandId && event.phase === "end") resolve(event);
    }));
    device.send(JSON.stringify({type:"command_state",commandId:"",pendingEvents:false}));
    assert.equal((await recovered).status, "interrupted");
    console.log("PASS conf lifecycle, ACK replay, and reconciliation on a connected socket after timeout");
  } finally {
    clearTimeout(timer);
    for (const socket of sockets) socket.terminate();
    server.kill();
  }
}

run().catch(error => { console.error(error); process.exitCode = 1; });
