import tls from "node:tls";

const SERVER_HOST = "127.0.0.1";
const SERVER_PORT = 8080;

const socket = tls.connect({
    host: SERVER_HOST,
    port: SERVER_PORT,
    rejectUnauthorized: false
});

socket.on("secureConnect", () => {
    console.log(`[Gateway] Connected to C++ server ${SERVER_HOST}:${SERVER_PORT}`);
});

socket.on("data", (chunk) => {
    console.log(`[Gateway] Received ${chunk.length} bytes`);
    console.log(chunk);
});

socket.on("error", (err) => {
    console.error("[Gateway] Socket error:", err.message);
});

socket.on("close", () => {
    console.log("[Gateway] C++ server connection closed");
});
