import { MessageType } from "../protocol/MessageType.js";
import { ServerNotification } from "../protocol/ProtoTypes.js";
import { decodeProto, protoToObject } from "../protocol/PacketCodec.js";


//=======================================================
// C++ -> Gateway -> Browser
//=======================================================

export function handleLoginPrompt(connection) {
    connection.sendBrowser({
        type: "login_prompt"
    });
}

export function handleServerNotification(connection, payload) {
    const notification = decodeProto(ServerNotification, payload);
    const data = protoToObject(ServerNotification, notification);

    connection.sendBrowser({
        type: "server_notification",
        ...data
    });
}

export function handlePong(connection) {
    connection.sendBrowser({
        type: "pong"
    });
}