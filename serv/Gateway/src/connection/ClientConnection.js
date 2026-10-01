import tls from "node:tls";
import { WebSocket } from "ws";
import { HEADER_SIZE, MAX_PACKET_SIZE, getPacketSize, decodePacket, serialize } from "../protocol/PacketCodec.js";


const SERVER_HOST = "127.0.0.1";
const SERVER_PORT = 8080;

export class ClientConnection {
    constructor(ws, dispatcher) {
        this.ws = ws;
        this.dispatcher = dispatcher;

        this.socket = null;
        this.receiveBuffer = Buffer.alloc(0);

        this.userId = 0;
        this.sequenceNumber = 1;
        this.reconnectToken = "";
    }

    start() {
        this.connectCppServer();
        this.setupWebSocket();
    }

    connectCppServer() {
        this.socket = tls.connect({
            host: SERVER_HOST,
            port: SERVER_PORT,
            rejectUnauthorized: false
        });

        this.socket.on("secureConnect", () => {
            console.log("[Gateway] Connected to C++ server");

            this.sendBrowser({
                type: "server_connected"
            });
        });

        this.socket.on("data", (chunk) => {
            this.receiveBuffer = Buffer.concat([
                this.receiveBuffer,
                chunk
            ]);

            this.processPackets();
        });

        this.socket.on("error", (err) => {
            console.error("[Gateway] C++ socket error:", err.message);

            this.sendBrowser({
                type: "server_error",
                errorMessage: err.message
            });
        });

        this.socket.on("close", () => {
            console.log("[Gateway] C++ server connection closed");

            this.sendBrowser({
                type: "server_disconnected"
            });
        });
    }

    setupWebSocket() {
        this.ws.on("message", async (data) => {
            try {
                const message = JSON.parse(data.toString());

                await this.dispatcher.dispatchBrowser(
                    this,
                    message
                );
            }
            catch (err) {
                console.error(
                    "[Gateway] Browser message error:",
                    err.message
                );

                this.sendBrowser({
                    type: "gateway_error",
                    errorMessage: err.message
                });
            }
        });

        this.ws.on("close", () => {
            console.log("[Gateway] Browser disconnected");

            if (this.socket && !this.socket.destroyed) {
                this.socket.destroy();
            }
        });

        this.ws.on("error", (err) => {
            console.error(
                "[Gateway] WebSocket error:",
                err.message
            );
        });
    }

    async processPackets() {
        while (
            this.receiveBuffer.length >= HEADER_SIZE
        ) {
            const packetSize =
                getPacketSize( //packetCodec
                    this.receiveBuffer
                );
            
            if (
                packetSize < HEADER_SIZE ||
                packetSize > MAX_PACKET_SIZE
            ) {
                console.error(
                    `[Gateway] Invalid packet size: ${packetSize}`
                );
            
                this.socket.destroy();
                return;
            }
        
            if (
                this.receiveBuffer.length <
                packetSize
            ) {
                return;
            }
        
            const packet =
                this.receiveBuffer.subarray(0, packetSize);
            
            this.receiveBuffer =
                this.receiveBuffer.subarray(packetSize);
            
            this.handlePacket(packet);
        }
    }
    
    async handlePacket(packet) {
        try {
            const { header, payload } = decodePacket(packet); //packetcodec
        
            await this.dispatcher.dispatchServer(
                this, header, payload
            );
        }
        catch (err) {
            console.error(
                "[Gateway] Server packet error:", err.message
            );
        }
    }
    send(messageType, protoType, value)
    {
        if ( !this.socket || this.socket.destroyed) {
            throw new Error(
                "C++ server is not connected"
            );
        }

        const packet = serialize(messageType, this.userId, this.sequenceNumber++,
        protoType, value); //packetCodec

        this.socket.write(packet);
    }

        sendBrowser(message) {
        if (this.ws.readyState !== WebSocket.OPEN) {
            return;
        }

        this.ws.send(JSON.stringify(message));
    }

    
}