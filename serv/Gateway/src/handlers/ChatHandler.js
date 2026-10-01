import { MessageType } from "../protocol/MessageType.js";
import {
    ChatMessage,
    ChatHistoryRequest, ChatHistoryResponse,
    WhisperRequest, WhisperResponse, WhisperNotification
} from "../protocol/ProtoTypes.js";
import { decodeProto, protoToObject } from "../protocol/PacketCodec.js";


//=======================================================
// Browser -> Gateway -> C++
//=======================================================

export function handleChat(connection, message) {
    connection.send(
        MessageType.CHAT_MESSAGE,
        ChatMessage,
        {
            roomId: message.roomId,
            message: message.message
        }
    );
}

export function handleChatHistory(connection, message) {
    connection.send(
        MessageType.CHAT_HISTORY_REQUEST,
        ChatHistoryRequest,
        {
            roomId: message.roomId,
            lastMessageId: message.lastMessageId ?? "0",
            count: message.count ?? 20
        }
    );
}

export function handleWhisper(connection, message) {
    connection.send(
        MessageType.WHISPER_REQUEST,
        WhisperRequest,
        {
            roomId: message.roomId,
            targetUsername: message.targetUsername,
            message: message.message
        }
    );
}


//=======================================================
// C++ -> Gateway -> Browser
//=======================================================

export function handleChatMessage(connection, payload) {
    const chatMessage = decodeProto(ChatMessage, payload);
    const data = protoToObject(ChatMessage, chatMessage);

    connection.sendBrowser({
        type: "chat_message",
        ...data
    });
}

export function handleChatHistoryResponse(connection, payload) {
    const response = decodeProto(ChatHistoryResponse, payload);
    const data = protoToObject(ChatHistoryResponse, response);

    connection.sendBrowser({
        type: "chat_history_response",
        ...data
    });
}

export function handleWhisperResponse(connection, payload) {
    const response = decodeProto(WhisperResponse, payload);
    const data = protoToObject(WhisperResponse, response);

    connection.sendBrowser({
        type: "whisper_response",
        ...data
    });
}

export function handleWhisperNotification(connection, payload) {
    const notification = decodeProto(WhisperNotification, payload);
    const data = protoToObject(WhisperNotification, notification);

    connection.sendBrowser({
        type: "whisper_notification",
        ...data
    });
}