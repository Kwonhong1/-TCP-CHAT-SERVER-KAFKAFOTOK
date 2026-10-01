import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";

import { ClientConnection } from "./connection/ClientConnection.js";
import { MessageDispatcher } from "./dispatcher/MessageDispatcher.js";
import { initHandlers } from "./InitHandlers.js";

const PORT = 8082;

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const PUBLIC_DIR = path.join(__dirname, "..", "public");


//=======================================================
// Dispatcher
//=======================================================

const dispatcher = new MessageDispatcher();
initHandlers(dispatcher);


//=======================================================
// HTTP Server
//=======================================================

const httpServer = http.createServer((req, res) => {
    let requestPath = req.url === "/" ? "/index.html" : req.url;
    requestPath = requestPath.split("?")[0];

    const filePath = path.resolve(PUBLIC_DIR, "." + requestPath);

    if (
        filePath !== PUBLIC_DIR &&
        !filePath.startsWith(PUBLIC_DIR + path.sep)
    ) {
        res.writeHead(403);
        res.end("Forbidden");
        return;
    }

    fs.readFile(filePath, (err, data) => {
        if (err) {
            res.writeHead(404, {
                "Content-Type": "text/plain; charset=utf-8"
            });

            res.end("Not Found");
            return;
        }

        res.writeHead(200, {
            "Content-Type": getContentType(filePath)
        });

        res.end(data);
    });
});


//=======================================================
// WebSocket Server
//=======================================================

const wss = new WebSocketServer({
    server: httpServer,
    path: "/ws"
});

wss.on("connection", (ws) => {
    console.log("[Gateway] Browser connected");

    const connection = new ClientConnection(ws, dispatcher);
    connection.start();
});


//=======================================================
// Start
//=======================================================

httpServer.listen(PORT, "0.0.0.0", () => {
    console.log(`[Gateway] HTTP + WebSocket server listening on port ${PORT}`);
});


//=======================================================
// Content Type
//=======================================================

function getContentType(filePath) {
    const ext = path.extname(filePath).toLowerCase();

    switch (ext) {
        case ".html":
            return "text/html; charset=utf-8";

        case ".css":
            return "text/css; charset=utf-8";

        case ".js":
            return "text/javascript; charset=utf-8";

        case ".json":
            return "application/json; charset=utf-8";

        case ".png":
            return "image/png";

        case ".jpg":
        case ".jpeg":
            return "image/jpeg";

        case ".svg":
            return "image/svg+xml";

        default:
            return "application/octet-stream";
    }
}