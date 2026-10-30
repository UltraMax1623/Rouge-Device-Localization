const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const { WebSocketServer, WebSocket } = require("ws");

const HOST = "0.0.0.0";
const PORT = 3000;
const PAGE_PATH = path.join(__dirname, "page.html");

// Keep this token identical to the one in the ESP32-S3 firmware.
const DEVICE_TOKEN = "local-device-token-change-me";

const allowedCommands = new Set([
  "conf",
  "optimize_rssi_an"
]);

let deviceSocket = null;

const recentLogs = [];
const MAX_RECENT_LOGS = 100;

const server = http.createServer((req, res) => {
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

  if (req.method === "POST" && req.url === "/api/command") {
    let body = "";
    let tooLarge = false;

    req.on("data", chunk => {
      body += chunk.toString();
      if (body.length > 100) tooLarge = true;
    });

    req.on("end", () => {
      if (tooLarge) {
        return sendText(res, 413, "Command is too long.");
      }

      const command = body.trim();

      if (!allowedCommands.has(command)) {
        return sendText(res, 400, "Unknown command.");
      }

      if (!deviceSocket || deviceSocket.readyState !== WebSocket.OPEN) {
        return sendText(res, 503, "ESP32-S3 is not connected to the backend.");
      }

      deviceSocket.send(command);
      logToUi(`Sent command "${command}" to ESP32-S3.`);

      return sendText(res, 202, `Command "${command}" sent to ESP32-S3.`);
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
    logToUi("ESP32-S3 connected by WebSocket.");
    broadcast({ type: "device_connection", connected: true });
  } else if (role !== "ui") {
    socket.close(1008, "Unknown connection role");
    return;
  }

  if (role === "ui") {
    socket.send(JSON.stringify({
      type: "device_connection",
      connected: Boolean(
        deviceSocket && deviceSocket.readyState === WebSocket.OPEN
      )
    }));

    for (const entry of recentLogs) {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify(entry));
      }
    }
  }

  socket.on("message", data => {
    if (role !== "device") return;

    try {
      const message = JSON.parse(data.toString());

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
      }
    } catch {
      logToUi("Ignored unrecognized message from ESP32-S3.", "ESP32");
    }
  });

  socket.on("close", () => {
    if (role === "device" && deviceSocket === socket) {
      deviceSocket = null;
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
  logToUi(`Backend running on port ${PORT}.`);
  logToUi("Open http://127.0.0.1:3000 on this PC.");
  logToUi("Keep page.html beside server.js.");
});