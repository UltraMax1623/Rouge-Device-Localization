const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const path = require("node:path");
const { WebSocket } = require("ws");

const server = spawn(process.execPath, [path.join(__dirname, "server.js")], {
  cwd: __dirname,
  stdio: ["ignore", "pipe", "pipe"]
});
const sockets = [];
const timeout = setTimeout(() => finish(new Error("WebSocket test timed out")), 5000);
let finished = false;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  for (const socket of sockets) socket.terminate();
  server.kill();
  if (error) {
    console.error(error);
    process.exitCode = 1;
  } else console.log("PASS backend relays mesh channel status");
}

server.once("error", finish);
server.stdout.on("data", output => {
  if (!output.toString().includes("Backend running")) return;
  const ui = new WebSocket("ws://127.0.0.1:3000/ws?role=ui");
  sockets.push(ui);
  ui.once("open", () => {
    const device = new WebSocket(
      "ws://127.0.0.1:3000/ws?role=device&token=local-device-token-change-me"
    );
    sockets.push(device);
    device.once("open", () => device.send(JSON.stringify({
      type: "mesh_channel", meshChannel: 11, routerChannel: 11
    })));
    device.once("error", finish);
  });
  ui.on("message", data => {
    const message = JSON.parse(data.toString());
    if (message.type !== "mesh_channel") return;
    try {
      assert.equal(message.meshChannel, 11);
      assert.equal(message.routerChannel, 11);
      finish();
    } catch (error) { finish(error); }
  });
  ui.once("error", finish);
});
