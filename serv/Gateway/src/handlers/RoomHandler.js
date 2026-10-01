import { MessageType } from "../protocol/MessageType.js";
import {
    CreateRoomRequest, CreateRoomResponse,
    RoomListRequest, RoomListResponse,
    JoinRoomRequest, JoinRoomResponse,
    LeaveRoomRequest, LeaveRoomResponse
} from "../protocol/ProtoTypes.js";
import { decodeProto, protoToObject } from "../protocol/PacketCodec.js";


//=======================================================
// Browser -> Gateway -> C++
//=======================================================

export function handleCreateRoom(connection, message) {
    connection.send(
        MessageType.CREATE_ROOM_REQUEST,
        CreateRoomRequest,
        {
            roomName: message.roomName,
            maxUsers: message.maxUsers
        }
    );
}

export function handleRoomList(connection) {
    connection.send(
        MessageType.ROOM_LIST_REQUEST,
        RoomListRequest,
        {}
    );
}

export function handleJoinRoom(connection, message) {
    connection.send(
        MessageType.JOIN_ROOM,
        JoinRoomRequest,
        {
            roomId: message.roomId
        }
    );
}

export function handleLeaveRoom(connection, message) {
    connection.send(
        MessageType.LEAVE_ROOM,
        LeaveRoomRequest,
        {
            roomId: message.roomId
        }
    );
}


//=======================================================
// C++ -> Gateway -> Browser
//=======================================================

export function handleCreateRoomResponse(connection, payload) {
    const response = decodeProto(CreateRoomResponse, payload);
    const data = protoToObject(CreateRoomResponse, response);

    connection.sendBrowser({
        type: "create_room_response",
        ...data
    });
}

export function handleRoomListResponse(connection, payload) {
    const response = decodeProto(RoomListResponse, payload);
    const data = protoToObject(RoomListResponse, response);

    connection.sendBrowser({
        type: "room_list_response",
        ...data
    });
}

export function handleJoinRoomResponse(connection, payload) {
    const response = decodeProto(JoinRoomResponse, payload);
    const data = protoToObject(JoinRoomResponse, response);

    connection.sendBrowser({
        type: "join_room_response",
        ...data
    });
}

export function handleLeaveRoomResponse(connection, payload) {
    const response = decodeProto(LeaveRoomResponse, payload);
    const data = protoToObject(LeaveRoomResponse, response);

    connection.sendBrowser({
        type: "leave_room_response",
        ...data
    });
}