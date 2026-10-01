export const HEADER_SIZE = 12;
export const MAX_PACKET_SIZE = 20 * 1024;


//=======================================================
// Serialize
//=======================================================

export function serialize(
    messageType,
    userId,
    sequenceNumber,
    protoType,
    value
) {
    const verifyError = protoType.verify(value);

    if (verifyError) {
        throw new Error(verifyError);
    }

    const message = protoType.create(value);
    const payload = Buffer.from(protoType.encode(message).finish());

    return makePacket(
        messageType,
        userId,
        sequenceNumber,
        payload
    );
}


//=======================================================
// Packet Encode
//=======================================================

function makePacket(
    messageType,
    userId,
    sequenceNumber,
    payload
) {
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


//=======================================================
// Packet Decode
//=======================================================

export function getPacketSize(buffer) {
    if (buffer.length < HEADER_SIZE) {
        throw new Error("Packet header is incomplete");
    }

    return buffer.readUInt16LE(0);
}

export function decodePacket(packet) {
    if (packet.length < HEADER_SIZE) {
        throw new Error("Packet is too small");
    }

    const packetSize = packet.readUInt16LE(0);

    if (packetSize !== packet.length) {
        throw new Error(
            `Packet size mismatch: header=${packetSize}, actual=${packet.length}`
        );
    }

    if (packetSize > MAX_PACKET_SIZE) {
        throw new Error(`Packet too large: ${packetSize}`);
    }

    const header = {
        packetSize,
        messageType: packet.readUInt16LE(2),
        userId: packet.readUInt32LE(4),
        sequenceNumber: packet.readUInt32LE(8)
    };

    const payload = packet.subarray(HEADER_SIZE);

    return {
        header,
        payload
    };
}


//=======================================================
// Protobuf Decode
//=======================================================

export function decodeProto(protoType, payload) {
    return protoType.decode(payload);
}

export function protoToObject(protoType, message) {
    return protoType.toObject(message, {
        longs: String,
        enums: String,
        bytes: String,
        defaults: true,
        arrays: true,
        objects: true
    });
}
