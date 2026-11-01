const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const { randomUUID } = require("node:crypto");
const { WebSocketServer, WebSocket } = require("ws");

const HOST = "0.0.0.0";
const PORT = Number(process.env.PORT || 3000);
const COMMAND_TIMEOUT_MS = Number(process.env.COMMAND_TIMEOUT_MS || 300000);
const PAGE_PATH = path.join(__dirname, "page.html");

// Keep this token identical to the one in the ESP32-S3 firmware.
const DEVICE_TOKEN = "local-device-token-change-me";

const { parseCommand, parseControl, names: allowedCommands } = require("./commands");

let deviceSocket = null;
let deviceReady = false;
let meshChannelStatus = null;
let activeCommandId = null;
let activeCommandName = "conf";
let networkStatus = null;
let pendingControl = null;
function sendPendingControl() {
  if (pendingControl && deviceReady && deviceSocket?.readyState === WebSocket.OPEN) deviceSocket.send(JSON.stringify(pendingControl));
}
setInterval(sendPendingControl, 2000).unref();
let activeCommandTimer = null;
function publishCommandLock() {
  broadcast({ type: "command_lock", locked: Boolean(activeCommandId), commandId: activeCommandId, command: activeCommandName });
}

const receivedEvents = new Set();
let commandHistory = [];

function finishTimedOutCommand() {
  if (!activeCommandId) return;
  const commandId = activeCommandId;
  activeCommandTimer = null;
  broadcast({ type: "command_event", commandId, command: activeCommandName, phase: "error",
    code: "OUTCOME_UNKNOWN", message: "Waiting for the root to reconnect and confirm the outcome." });
}

const recentLogs = [];
const MAX_RECENT_LOGS = 100;

const server = http.createServer((req, res) => {
  const assets = {
    "/assets/cytoscape.js": path.join(__dirname, "node_modules/cytoscape/dist/cytoscape.min.js"),
    "/assets/node-map.js": path.join(__dirname, "node-map.js"),
    "/assets/dashboard.js": path.join(__dirname, "dashboard.js")
  };
  if (req.method === "GET" && assets[req.url]) {
    if (!fs.existsSync(assets[req.url])) return sendText(res, 404, "Run npm install in UI.");
    res.writeHead(200, { "Content-Type": "application/javascript; charset=utf-8" });
    return fs.createReadStream(assets[req.url]).pipe(res);
  }
  if (req.method === "GET" && req.url === "/") {
    if (!fs.existsSync(PAGE_PATH)) {
      return sendText(res, 404, "Put page.html beside server.js.");
    }

    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    return fs.createReadStream(PAGE_PATH).pipe(res);
  }

  if (req.method === "GET" && req.url === "/api/health") {
    return sendText(res, 200, "Backend is running.");
  }

  if (req.method === "POST" && req.url === "/api/control") {
    let body = "", oversized = false;
    req.on("data", chunk => { if (body.length + chunk.length > 512) oversized = true; if (!oversized) body += chunk; });
    req.on("end", () => {
      if (oversized) return sendText(res, 413, "Action is too long.");
      let action;
      try { action = parseControl(body, activeCommandId, activeCommandName); }
      catch (error) { return sendText(res, 409, error.message); }
      if (pendingControl && action !== "cancel") return sendText(res, 409, "Waiting for the previous action acknowledgement.");
      pendingControl = { type: "command_control", commandId: activeCommandId, action, controlId: randomUUID() };
      sendPendingControl();
      res.writeHead(202, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ status: "sent", controlId: pendingControl.controlId }));
    });
    return;
  }

  if (req.method === "POST" && req.url === "/api/command") {
    let body = "";
    let tooLarge = false;

    req.on("data", chunk => {
      if (body.length + chunk.length > 2048) tooLarge = true;
      if (!tooLarge) body += chunk.toString();
    });

    req.on("end", () => {
      if (tooLarge) {
        return sendText(res, 413, "Command is too long.");
      }

      let parsed;
      try { parsed = parseCommand(body, (req.headers["content-type"] || "").includes("application/json")); }
      catch (error) { return sendText(res, 400, error.message); }
      const { command, params } = parsed;

      if (!deviceSocket || deviceSocket.readyState !== WebSocket.OPEN || !deviceReady) {
        return sendText(res, 503, "Root is offline or restoring its command state. Wait for reconnection.");
      }
      if (activeCommandId) {
        return sendText(res, 409, "A command is already running.");
      }

      const commandId = randomUUID();
      commandHistory = [];
      activeCommandId = commandId;
      activeCommandName = command;
      pendingControl = null;
      publishCommandLock();
      activeCommandTimer = setTimeout(finishTimedOutCommand, COMMAND_TIMEOUT_MS);
      deviceSocket.send(JSON.stringify({ type: "command", command, commandId, params }), error => {
        if (error && activeCommandId === commandId) {
          activeCommandId = null;
          publishCommandLock();
          clearTimeout(activeCommandTimer);
          activeCommandTimer = null;
          broadcast({ type: "command_event", commandId, command, phase: "error",
            code: "SEND_FAILED", message: "Could not send command to the root." });
          broadcast({ type: "command_event", commandId, command, phase: "end", status: "failed" });
        }
      });
      logToUi(`Sent command "${command}" (${commandId}) to ESP32-S3.`);

      res.writeHead(202, { "Content-Type": "application/json; charset=utf-8" });
      return res.end(JSON.stringify({ command, commandId, status: "sent" }));
    });

    return;
  }

  return sendText(res, 404, "Not found.");
});

const webSockets = new WebSocketServer({
  server,
  path: "/ws"
});

webSockets.on("connection", (socket, request) => {
  socket.on("error", () => logToUi("WebSocket transport error; waiting for reconnection."));
  const url = new URL(request.url, `http://${request.headers.host}`);
  const role = url.searchParams.get("role");

  if (role === "device") {
    const token = url.searchParams.get("token");

    if (token !== DEVICE_TOKEN) {
      socket.close(1008, "Invalid device token");
      return;
    }

    if (deviceSocket && deviceSocket.readyState === WebSocket.OPEN) {
      deviceSocket.close();
    }

    deviceSocket = socket;
    deviceReady = false;
    meshChannelStatus = null;
    logToUi("ESP32-S3 connected by WebSocket.");
  } else if (role !== "ui") {
    socket.close(1008, "Unknown connection role");
    return;
  }

  if (role === "ui") {
    for (const event of commandHistory) socket.send(JSON.stringify(event));
    socket.send(JSON.stringify({ type: "command_lock", locked: Boolean(activeCommandId), commandId: activeCommandId, command: activeCommandName }));
    socket.send(JSON.stringify({
      type: "device_connection",
      connected: Boolean(
        deviceSocket && deviceSocket.readyState === WebSocket.OPEN && deviceReady
      )
    }));
    if (meshChannelStatus) socket.send(JSON.stringify(meshChannelStatus));
    if (networkStatus) socket.send(JSON.stringify(networkStatus));

    for (const entry of recentLogs) {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify(entry));
      }
    }
  }

  socket.on("message", data => {
    if (role !== "device" || socket !== deviceSocket) return;

    try {
      const message = JSON.parse(data.toString());

      if (message.type === "network_status") {
        networkStatus = message; broadcast(message); return;
      }
      if (message.type === "control_ack") {
        if (pendingControl && message.controlId === pendingControl.controlId) {
          pendingControl = null; broadcast(message);
        }
        return;
      }
      if (message.type === "command_state") {
        if (!activeCommandId && typeof message.commandId === "string" && message.commandId) {
          activeCommandId = message.commandId;
          activeCommandName = allowedCommands.has(message.command) ? message.command : "conf";
          publishCommandLock();
          activeCommandTimer = setTimeout(finishTimedOutCommand, COMMAND_TIMEOUT_MS);
        }
        if (activeCommandId && !message.commandId && !message.pendingEvents) {
          const ended = { type: "command_event", commandId: activeCommandId,
            command: activeCommandName, phase: "end", status: "interrupted",
            message: "The root has no active command or retained result; it may have restarted." };
          commandHistory.push(ended);
          broadcast(ended);
          pendingControl = null;
          activeCommandId = null;
          publishCommandLock();
          clearTimeout(activeCommandTimer);
        }
        deviceReady = true;
        socket.send(JSON.stringify({ type: "state_ack" }));
        broadcast({ type: "device_connection", connected: true });
        sendPendingControl();
        return;
      }

      if (message.type === "command_event" &&
          allowedCommands.has(message.command) &&
          ["start", "log", "error", "data", "end"].includes(message.phase)) {
        const key = `${message.commandId}:${message.sequence}`;
        if (Number.isInteger(message.sequence)) {
          socket.send(JSON.stringify({ type: "event_ack", commandId: message.commandId,
            sequence: message.sequence }));
          if (receivedEvents.has(key)) return;
        }
        if (!activeCommandId) {
          activeCommandId = message.commandId;
          activeCommandName = allowedCommands.has(message.command) ? message.command : "conf";
          publishCommandLock();
          activeCommandTimer = setTimeout(finishTimedOutCommand, COMMAND_TIMEOUT_MS);
        }
        if (message.commandId !== activeCommandId) return;
        if (Number.isInteger(message.sequence)) {
          receivedEvents.add(key);
          if (receivedEvents.size > 2048) receivedEvents.delete(receivedEvents.values().next().value);
        }
        commandHistory.push(message);
        broadcast(message);
        clearTimeout(activeCommandTimer);
        activeCommandTimer = setTimeout(finishTimedOutCommand, COMMAND_TIMEOUT_MS);
        if (message.phase === "end") {
          pendingControl = null;
          activeCommandId = null;
          publishCommandLock();
          clearTimeout(activeCommandTimer);
          activeCommandTimer = null;
        }
        return;
      }

      if (message.type === "log") {
        const text =
          typeof message.message === "string"
            ? message.message.slice(0, 500)
            : "ESP32 sent a log message.";

        logToUi(text, "ESP32");
        return;
      }

      if (
        message.type === "node_status" ||
        message.type === "rssi_optimization"
      ) {
        broadcast(message);
        logToUi(`ESP32 update: ${JSON.stringify(message)}`, "ESP32");
        return;
      }
      if (message.type === "mesh_channel" &&
          Number.isInteger(message.meshChannel) && message.meshChannel >= 1 &&
          message.meshChannel <= 13 &&
          Number.isInteger(message.routerChannel) && message.routerChannel >= 0 &&
          message.routerChannel <= 13) {
        meshChannelStatus = message;
        broadcast(message);
        logToUi(`Mesh channel ${message.meshChannel}; Pavan channel ${message.routerChannel || "unknown"}.`, "ESP32");
      }
    } catch {
      logToUi("Ignored unrecognized message from ESP32-S3.", "ESP32");
    }
  });

  socket.on("close", () => {
    if (role === "device" && deviceSocket === socket) {
      deviceSocket = null;
      deviceReady = false;
      meshChannelStatus = null;
      logToUi("ESP32-S3 disconnected.");
      broadcast({ type: "device_connection", connected: false });
    }
  });
});

function logToUi(message, source = "BACKEND") {
  console.log(message);

  const entry = {
    type: "log",
    source,
    message,
    timestamp: new Date().toISOString()
  };

  recentLogs.push(entry);

  if (recentLogs.length > MAX_RECENT_LOGS) {
    recentLogs.shift();
  }

  broadcast(entry);
}

function broadcast(message) {
  const data = JSON.stringify(message);

  for (const client of webSockets.clients) {
    if (
      client !== deviceSocket &&
      client.readyState === WebSocket.OPEN
    ) {
      client.send(data);
    }
  }
}

function sendText(res, statusCode, text) {
  res.writeHead(statusCode, {
    "Content-Type": "text/plain; charset=utf-8",
    "Cache-Control": "no-store"
  });
  res.end(text);
}

server.listen(PORT, HOST, () => {
  const actualPort = server.address().port;
  logToUi(`Backend running on port ${actualPort}.`);
  logToUi(`Open http://127.0.0.1:${actualPort} on this PC.`);
  logToUi("Keep page.html beside server.js.");
});
