import { send, on } from "./socket.js";

let currentRoomId = 0;
let currentOwnerId = 0;
let oldestMessageId = "0";
let leaveHandler = null;

export function initChat(onLeaveRoom) {
    leaveHandler = onLeaveRoom;

    document
        .getElementById("send-chat-button")
        .addEventListener("click", sendChat);

    document
        .getElementById("chat-message-input")
        .addEventListener("keydown", (event) => {
            if (event.key === "Enter") {
                sendChat();
            }
        });

    document
        .getElementById("leave-room-button")
        .addEventListener("click", leaveRoom);

    document
        .getElementById("load-history-button")
        .addEventListener("click", loadHistory);

    document
        .getElementById("whisper-button")
        .addEventListener("click", sendWhisper);

    document
        .getElementById("kick-user-button")
        .addEventListener("click", kickUser);

    document
        .getElementById("transfer-master-button")
        .addEventListener("click", transferMaster);

    on("chat_message", handleChatMessage);
    on("chat_history_response", handleHistory);
    on("leave_room_response", handleLeaveResponse);

    on("whisper_response", handleWhisperResponse);
    on("whisper_notification", handleWhisperNotification);

    on("kick_user_response", handleKickResponse);
    on("kicked_notification", handleKicked);

    on(
        "transfer_master_response",
        handleTransferResponse
    );

    on(
        "master_changed_notification",
        handleMasterChanged
    );

    on(
        "server_notification",
        handleServerNotification
    );
}

export function enterRoom(room) {
    currentRoomId = room.roomId;
    currentOwnerId = room.ownerId;
    oldestMessageId = "0";

    document.getElementById("chat-room-title").textContent =
        room.roomName || `Room ${room.roomId}`;

    updateRoomInfo();

    const messages =
        document.getElementById("chat-messages");

    messages.innerHTML = "";

    for (const message of room.recentMessages || []) {
        appendChatMessage(message);
    }
}

function sendChat() {
    const input =
        document.getElementById(
            "chat-message-input"
        );

    const message = input.value.trim();

    if (!message || !currentRoomId) {
        return;
    }

    send({
        type: "chat",
        roomId: currentRoomId,
        message
    });

    input.value = "";
}

function leaveRoom() {
    if (!currentRoomId) {
        return;
    }

    send({
        type: "leave_room",
        roomId: currentRoomId
    });
}

function loadHistory() {
    if (!currentRoomId) {
        return;
    }

    send({
        type: "chat_history",
        roomId: currentRoomId,
        lastMessageId: oldestMessageId,
        count: 20
    });
}

function sendWhisper() {
    const targetUsername =
        document
            .getElementById("whisper-target")
            .value
            .trim();

    const message =
        document
            .getElementById("whisper-message")
            .value
            .trim();

    if (!targetUsername || !message) {
        return;
    }

    send({
        type: "whisper",
        roomId: currentRoomId,
        targetUsername,
        message
    });

    document.getElementById(
        "whisper-message"
    ).value = "";
}

function kickUser() {
    const targetUserId = Number(
        document
            .getElementById("kick-user-id")
            .value
    );

    if (!Number.isInteger(targetUserId)) {
        return;
    }

    send({
        type: "kick_user",
        roomId: currentRoomId,
        targetUserId
    });
}

function transferMaster() {
    const newMasterId = Number(
        document
            .getElementById("master-user-id")
            .value
    );

    if (!Number.isInteger(newMasterId)) {
        return;
    }

    send({
        type: "transfer_master",
        roomId: currentRoomId,
        newMasterId
    });
}

function handleChatMessage(message) {
    if (message.roomId !== currentRoomId) {
        return;
    }

    appendChatMessage(message);
}

function handleHistory(message) {
    if (
        !message.success ||
        message.roomId !== currentRoomId
    ) {
        return;
    }

    const container =
        document.getElementById(
            "chat-messages"
        );

    for (
        const chatMessage of
        [...message.messages].reverse()
    ) {
        const element =
            createChatMessage(chatMessage);

        container.prepend(element);
    }

    if (message.messages.length > 0) {
        oldestMessageId =
            message.messages[
                message.messages.length - 1
            ].messageId;
    }
}

function handleLeaveResponse(message) {
    if (!message.success) {
        addNotification(
            message.errorMessage ||
            "방 나가기에 실패했습니다."
        );

        return;
    }

    exitRoom();
}

function handleWhisperResponse(message) {
    if (!message.success) {
        addNotification(
            message.errorMessage ||
            "귓속말 전송에 실패했습니다."
        );
    }
}

function handleWhisperNotification(message) {
    addNotification(
        `[귓속말] ${message.senderUsername}: ${message.message}`
    );
}

function handleKickResponse(message) {
    if (!message.success) {
        addNotification(
            message.errorMessage ||
            "강퇴에 실패했습니다."
        );
    }
}

function handleKicked(message) {
    if (message.roomId !== currentRoomId) {
        return;
    }

    addNotification(
        message.reason ||
        "방에서 강퇴되었습니다."
    );

    exitRoom();
}

function handleTransferResponse(message) {
    if (!message.success) {
        addNotification(
            message.errorMessage ||
            "방장 위임에 실패했습니다."
        );
    }
}

function handleMasterChanged(message) {
    if (message.roomId !== currentRoomId) {
        return;
    }

    currentOwnerId = message.newMasterId;

    updateRoomInfo();

    addNotification(
        `방장이 User ${message.newMasterId}로 변경되었습니다.`
    );
}

function handleServerNotification(message) {
    addNotification(message.message);
}

function appendChatMessage(message) {
    const container =
        document.getElementById(
            "chat-messages"
        );

    container.appendChild(
        createChatMessage(message)
    );

    container.scrollTop =
        container.scrollHeight;

    if (
        oldestMessageId === "0" &&
        message.messageId
    ) {
        oldestMessageId =
            message.messageId;
    }
}

function createChatMessage(message) {
    const element =
        document.createElement("div");

    element.className = "message";

    const header =
        document.createElement("div");

    header.className = "message-header";

    const date =
        message.timestamp
            ? new Date(
                Number(message.timestamp) * 1000
            )
            : null;

    header.textContent =
        `${message.senderUsername || "Unknown"} ` +
        `(User ${message.senderId})` +
        (date
            ? ` · ${date.toLocaleTimeString()}`
            : "");

    const content =
        document.createElement("div");

    content.className = "message-content";
    content.textContent = message.message;

    element.append(header, content);

    return element;
}

function updateRoomInfo() {
    document.getElementById(
        "room-info"
    ).textContent =
        `Room ID: ${currentRoomId} · Owner: ${currentOwnerId}`;
}

function addNotification(text) {
    const container =
        document.getElementById(
            "notifications"
        );

    const item =
        document.createElement("div");

    item.className = "notification";
    item.textContent = text;

    container.prepend(item);
}

function exitRoom() {
    currentRoomId = 0;
    currentOwnerId = 0;
    oldestMessageId = "0";

    document.getElementById(
        "chat-messages"
    ).innerHTML = "";

    if (leaveHandler) {
        leaveHandler();
    }
}