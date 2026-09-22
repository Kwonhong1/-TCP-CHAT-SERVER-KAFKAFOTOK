import tls from "node:tls";
import protobuf from "protobufjs";
const SERVER_HOST = "127.0.0.1";
const SERVER_PORT = 8080;
const HEADER_SIZE = 12;
const MAX_PACKET_SIZE = 20 * 1024;




const root = await protobuf.load("../chat_protocol.proto");


const LoginRequest = root.lookupType("chat.LoginRequest");
const LoginResponse = root.lookupType("chat.LoginResponse");

let receiveBuffer = Buffer.alloc(0);

const socket = tls.connect({
    host: SERVER_HOST,
    port: SERVER_PORT,
    rejectUnauthorized: false
});

socket.on("secureConnect", () => {
    console.log(`[Gateway] Connected to C++ server ${SERVER_HOST}:${SERVER_PORT}`);
});

socket.on("data", (chunk) => {
    console.log(`[Gateway] Received chunk: ${chunk.length} bytes`);

    receiveBuffer = Buffer.concat([receiveBuffer, chunk]);

    processPacket();
});

socket.on("error", (err) => {
    console.error("[Gateway] Socket error:", err.message);
});

socket.on("close", () => {
    console.log("[Gateway] C++ server connection closed");
});

function processPacket() {
    while (receiveBuffer.length >= HEADER_SIZE) {
        const packetSize = receiveBuffer.readUInt16LE(0);

        if (packetSize < HEADER_SIZE || packetSize > MAX_PACKET_SIZE) {
            console.error(`[Gateway] Invalid packet size: ${packetSize}`);
            socket.destroy();
            return;
        }

        if (receiveBuffer.length < packetSize) {
            return;
        }

        const packet = receiveBuffer.subarray(0, packetSize);
        receiveBuffer = receiveBuffer.subarray(packetSize);

        handlePacket(packet);
    }
}

function handlePacket(packet) {
    const packetSize = packet.readUInt16LE(0);
    const messageType = packet.readUInt16LE(2);
    const userId = packet.readUInt32LE(4);
    const sequenceNumber = packet.readUInt32LE(8);

    const payload = packet.subarray(HEADER_SIZE);

    console.log("[Gateway] Packet");
    console.log(`  size     : ${packetSize}`);
    console.log(`  type     : ${messageType}`);
    console.log(`  userId   : ${userId}`);
    console.log(`  sequence : ${sequenceNumber}`);
    console.log(`  payload  : ${payload.length} bytes`);

    switch (messageType) {
        case 1000:
            console.log("[Gateway] LOGIN_PROMPT received");

            sendLogin("test", "1234");
            break;

        case 1002:
            handleLoginResponse(payload);
            break;

        default:
            console.log(`[Gateway] Unknown/unhandled message type: ${messageType}`);
            break;
    }
}

function makePacket(messageType, userId, sequenceNumber, payload = Buffer.alloc(0)) {
    const packetSize = HEADER_SIZE + payload.length;

    if (packetSize > MAX_PACKET_SIZE) {
        throw new Error(`Packet too large: ${packetSize}`);
    }

    const packet = Buffer.alloc(packetSize);

    packet.writeUInt16LE(packetSize, 0);
    packet.writeUInt16LE(messageType, 2);
    packet.writeUInt32LE(userId, 4);
    packet.writeUInt32LE(sequenceNumber, 8);

    payload.copy(packet, HEADER_SIZE);

    return packet;
}


function sendLogin(username, password, reconnectToken = "") {
    const message = LoginRequest.create({
        username,
        password,
        reconnectToken
    });
    message.username="hololo";
    message.password="1324";
    message.reconnectToken="";
    const payload = LoginRequest.encode(message).finish();

    const packet = makePacket(
        1001,
        0,
        1,
        Buffer.from(payload)
    );

    console.log(`[Gateway] Sending LOGIN_REQUEST: ${packet.length} bytes`);

    socket.write(packet);
}

function handleLoginResponse(payload) {
    try {
        const response = LoginResponse.decode(payload);

        console.log("[Gateway] LOGIN_RESPONSE");
        console.log(`  success        : ${response.success}`);
        console.log(`  assignedUserId : ${response.assignedUserId}`);
        console.log(`  reconnectToken : ${response.reconnectToken}`);
        console.log(`  errorMessage   : ${response.errorMessage}`);
    } catch (err) {
        console.error("[Gateway] Failed to decode LoginResponse:", err.message);
    }
}
