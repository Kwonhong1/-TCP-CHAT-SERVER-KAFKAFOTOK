import { send, on } from "./socket.js";

let joinHandler = null;

export function initLobby(onJoinedRoom) {
    joinHandler = onJoinedRoom;

    document
        .getElementById("refresh-rooms-button")
        .addEventListener("click", requestRoomList);

    document
        .getElementById("create-room-button")
        .addEventListener("click", createRoom);

    on("room_list_response", handleRoomList);
    on("create_room_response", handleCreateRoomResponse);
    on("join_room_response", handleJoinRoomResponse);
}

export function requestRoomList() {
    send({
        type: "room_list"
    });
}

function createRoom() {
    const roomName = document.getElementById("room-name").value.trim();

    const maxUsers = Number(
        document.getElementById("room-max-users").value
    );

    if (!roomName) {
        showMessage("방 이름을 입력하세요.");
        return;
    }

    if (
        !Number.isInteger(maxUsers) ||
        maxUsers < 2
    ) {
        showMessage("최대 인원을 확인하세요.");
        return;
    }

    send({
        type: "create_room",
        roomName,
        maxUsers
    });
}

function joinRoom(roomId, roomName) {
    send({
        type: "join_room",
        roomId
    });

    sessionStorage.setItem(
        "pendingRoomName",
        roomName
    );
}

function handleRoomList(message) {
    const container =
        document.getElementById("room-list");

    container.innerHTML = "";

    if (!message.rooms?.length) {
        container.textContent =
            "생성된 방이 없습니다.";

        return;
    }

    for (const room of message.rooms) {
        const item =
            document.createElement("div");

        item.className = "room-item";

        const info =
            document.createElement("div");

        info.className = "room-info";

        const name =
            document.createElement("span");

        name.className = "room-name";
        name.textContent = room.roomName;

        const users =
            document.createElement("span");

        users.className = "room-users";

        users.textContent =
            `${room.currentUsers} / ${room.maxUsers}`;

        const button =
            document.createElement("button");

        button.textContent = "입장";

        button.addEventListener(
            "click",
            () => joinRoom(
                room.roomId,
                room.roomName
            )
        );

        info.append(name, users);
        item.append(info, button);

        container.appendChild(item);
    }
}

function handleCreateRoomResponse(message) {
    if (!message.success) {
        showMessage(
            message.errorMessage ||
            "방 생성에 실패했습니다."
        );

        return;
    }

    showMessage("");

    requestRoomList();

    /*
     * 현재 C++ 서버가 방 생성자를 자동으로
     * 입장시키는 구조라면 여기서 바로
     * 채팅 화면으로 이동할 수 있다.
     */

    if (joinHandler) {
        joinHandler({
            roomId: message.createdRoomId,
            ownerId: message.ownerId,
            roomName:
                document
                    .getElementById("room-name")
                    .value
                    .trim(),
            recentMessages: []
        });
    }
}

function handleJoinRoomResponse(message) {
    if (!message.success) {
        showMessage(
            message.errorMessage ||
            "방 입장에 실패했습니다."
        );

        return;
    }

    const roomName =
        sessionStorage.getItem(
            "pendingRoomName"
        ) || `Room ${message.roomId}`;

    sessionStorage.removeItem(
        "pendingRoomName"
    );

    if (joinHandler) {
        joinHandler({
            ...message,
            roomName
        });
    }
}

function showMessage(message) {
    document.getElementById("lobby-message").textContent = message;
}