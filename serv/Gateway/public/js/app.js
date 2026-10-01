import {
    connect,
    on
} from "./socket.js";

import {
    initAuth
} from "./auth.js";

import {
    initLobby,
    requestRoomList
} from "./lobby.js";

import {
    initChat,
    enterRoom
} from "./chat.js";


let currentUserId = 0;


//=======================================================
// Init
//=======================================================

initAuth(handleLoginSuccess);
initLobby(handleJoinedRoom);
initChat(handleLeaveRoom);


//=======================================================
// Gateway State
//=======================================================

on("gateway_connected", () => {
    setConnectionStatus(
        true,
        "Gateway 연결됨"
    );
});

on("gateway_disconnected", () => {
    setConnectionStatus(
        false,
        "Gateway 연결 끊김"
    );
});

on("server_connected", () => {
    setConnectionStatus(
        true,
        "C++ 서버 연결됨"
    );
});

on("server_disconnected", () => {
    setConnectionStatus(
        false,
        "C++ 서버 연결 끊김"
    );
});

on("gateway_error", (message) => {
    console.error(
        "Gateway:",
        message.errorMessage
    );
});

on("server_error", (message) => {
    console.error(
        "Server:",
        message.errorMessage
    );
});


//=======================================================
// View Flow
//=======================================================

function handleLoginSuccess(message) {
    currentUserId =
        message.assignedUserId;

    document.getElementById(
        "user-info"
    ).textContent =
        `User ID: ${currentUserId}`;

    showView("lobby-view");

    requestRoomList();
}

function handleJoinedRoom(room) {
    enterRoom(room);
    showView("chat-view");
}

function handleLeaveRoom() {
    showView("lobby-view");
    requestRoomList();
}


//=======================================================
// UI
//=======================================================

function showView(viewId) {
    document
        .querySelectorAll(".view")
        .forEach((view) => {
            view.classList.remove("active");
        });

    document
        .getElementById(viewId)
        .classList.add("active");
}

function setConnectionStatus(
    connected,
    text
) {
    const element =
        document.getElementById(
            "connection-status"
        );

    element.textContent = text;

    element.classList.toggle(
        "connected",
        connected
    );

    element.classList.toggle(
        "disconnected",
        !connected
    );
}


//=======================================================
// Start
//=======================================================

connect();