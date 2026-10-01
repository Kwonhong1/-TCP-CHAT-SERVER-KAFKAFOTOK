export const HEADER_SIZE = 12;
export const MAX_PACKET_SIZE = 20 * 1024;

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

    const payload = Buffer.from(
        protoType.encode(message).finish()
    );

    return makePacket(
        messageType,
        userId,
        sequenceNumber,
        payload
    );
}

function makePacket(
    messageType,
    userId,
    sequenceNumber,
    payload
) {
    const packetSize = HEADER_SIZE + payload.length;

    if (packetSize > MAX_PACKET_SIZE) {
        throw new Error(
            `Packet too large: ${packetSize}`
        );
    }

    const packet = Buffer.alloc(packetSize);

    packet.writeUInt16LE(packetSize, 0);
    packet.writeUInt16LE(messageType, 2);
    packet.writeUInt32LE(userId, 4);
    packet.writeUInt32LE(sequenceNumber, 8);

    payload.copy(packet, HEADER_SIZE);

    return packet;
}

//proto용
export function protoToObject(
    protoType,
    message
) {
    return protoType.toObject(message, {
        longs: String,
        enums: String,
        bytes: String,
        defaults: true,
        arrays: true,
        objects: true
    });
}