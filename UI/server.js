const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const { WebSocketServer, WebSocket } = require("ws");

const HOST = "0.0.0.0";
const PORT = 3000;
const PAGE_PATH = path.join(__dirname, "page.html");

// Use this same token in the ESP32-S3 firmware later.
const DEVICE_TOKEN = "local-device-token-change-me";

const allowedCommands = new Set([
  "conf",
  "optimize_rssi_an"
]);

let deviceSocket = null;

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
      console.log(`Sent command "${command}" to ESP32-S3.`);
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
    console.log("ESP32-S3 connected by WebSocket.");
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
  }

  socket.on("message", data => {
    if (role !== "device") return;

    try {
      const message = JSON.parse(data.toString());

      if (
        message.type === "node_status" ||
        message.type === "rssi_optimization"
      ) {
        broadcast(message);
        console.log("ESP32 update:", message);
      }
    } catch {
      console.log("Ignored unrecognized message from ESP32-S3.");
    }
  });

  socket.on("close", () => {
    if (role === "device" && deviceSocket === socket) {
      deviceSocket = null;
      console.log("ESP32-S3 disconnected.");
      broadcast({ type: "device_connection", connected: false });
    }
  });
});

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
  console.log(`Backend running on port ${PORT}.`);
  console.log("Open http://127.0.0.1:3000 on this PC.");
  console.log("Keep page.html beside server.js.");
});