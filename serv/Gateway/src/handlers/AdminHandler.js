import { MessageType } from "../protocol/MessageType.js";
import {
    KickUserRequest, KickUserResponse, KickedNotification,
    TransferMasterRequest, TransferMasterResponse, MasterChangedNotification
} from "../protocol/ProtoTypes.js";
import { decodeProto, protoToObject } from "../protocol/PacketCodec.js";


//=======================================================
// Browser -> Gateway -> C++
//=======================================================

export function handleKickUser(connection, message) {
    connection.send(
        MessageType.KICK_USER_REQUEST,
        KickUserRequest,
        {
            roomId: message.roomId,
            targetUserId: message.targetUserId
        }
    );
}

export function handleTransferMaster(connection, message) {
    connection.send(
        MessageType.TRANSFER_MASTER_REQUEST,
        TransferMasterRequest,
        {
            roomId: message.roomId,
            newMasterId: message.newMasterId
        }
    );
}


//=======================================================
// C++ -> Gateway -> Browser
//=======================================================

export function handleKickUserResponse(connection, payload) {
    const response = decodeProto(KickUserResponse, payload);
    const data = protoToObject(KickUserResponse, response);

    connection.sendBrowser({
        type: "kick_user_response",
        ...data
    });
}

export function handleKickedNotification(connection, payload) {
    const notification = decodeProto(KickedNotification, payload);
    const data = protoToObject(KickedNotification, notification);

    connection.sendBrowser({
        type: "kicked_notification",
        ...data
    });
}

export function handleTransferMasterResponse(connection, payload) {
    const response = decodeProto(TransferMasterResponse, payload);
    const data = protoToObject(TransferMasterResponse, response);

    connection.sendBrowser({
        type: "transfer_master_response",
        ...data
    });
}

export function handleMasterChangedNotification(connection, payload) {
    const notification = decodeProto(MasterChangedNotification, payload);
    const data = protoToObject(MasterChangedNotification, notification);

    connection.sendBrowser({
        type: "master_changed_notification",
        ...data
    });
}
