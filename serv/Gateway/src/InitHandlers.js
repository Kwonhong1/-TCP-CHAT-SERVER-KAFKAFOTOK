import { MessageType } from "./protocol/MessageType.js";

import * as AuthHandler from "./handlers/AuthHandler.js";
import * as RoomHandler from "./handlers/RoomHandler.js";
import * as ChatHandler from "./handlers/ChatHandler.js";
import * as AdminHandler from "./handlers/AdminHandler.js";
import * as SystemHandler from "./handlers/SystemHandler.js";

export function initHandlers(dispatcher) {
    // Browser -> C++
    dispatcher.registerBrowser("login", AuthHandler.handleLogin);
    dispatcher.registerBrowser("register", AuthHandler.handleRegister);

    dispatcher.registerBrowser("create_room", RoomHandler.handleCreateRoom);
    dispatcher.registerBrowser("room_list", RoomHandler.handleRoomList);
    dispatcher.registerBrowser("join_room", RoomHandler.handleJoinRoom);
    dispatcher.registerBrowser("leave_room", RoomHandler.handleLeaveRoom);

    dispatcher.registerBrowser("chat", ChatHandler.handleChat);
    dispatcher.registerBrowser("chat_history", ChatHandler.handleChatHistory);
    dispatcher.registerBrowser("whisper", ChatHandler.handleWhisper);

    dispatcher.registerBrowser("kick_user", AdminHandler.handleKickUser);
    dispatcher.registerBrowser("transfer_master", AdminHandler.handleTransferMaster);

    // C++ -> Browser
    dispatcher.registerServer(MessageType.LOGIN_PROMPT, SystemHandler.handleLoginPrompt);
    dispatcher.registerServer(MessageType.LOGIN_RESPONSE, AuthHandler.handleLoginResponse);
    dispatcher.registerServer(MessageType.REGISTER_RESPONSE, AuthHandler.handleRegisterResponse);

    dispatcher.registerServer(MessageType.CREATE_ROOM_RESPONSE, RoomHandler.handleCreateRoomResponse);
    dispatcher.registerServer(MessageType.ROOM_LIST_RESPONSE, RoomHandler.handleRoomListResponse);
    dispatcher.registerServer(MessageType.JOIN_ROOM_RESPONSE, RoomHandler.handleJoinRoomResponse);
    dispatcher.registerServer(MessageType.LEAVE_ROOM_RESPONSE, RoomHandler.handleLeaveRoomResponse);

    dispatcher.registerServer(MessageType.CHAT_MESSAGE, ChatHandler.handleChatMessage);
    dispatcher.registerServer(MessageType.CHAT_HISTORY_RESPONSE, ChatHandler.handleChatHistoryResponse);
    dispatcher.registerServer(MessageType.WHISPER_RESPONSE, ChatHandler.handleWhisperResponse);
    dispatcher.registerServer(MessageType.WHISPER_NOTIFICATION, ChatHandler.handleWhisperNotification);

    dispatcher.registerServer(MessageType.KICK_USER_RESPONSE, AdminHandler.handleKickUserResponse);
    dispatcher.registerServer(MessageType.KICKED_NOTIFICATION, AdminHandler.handleKickedNotification);
    dispatcher.registerServer(MessageType.TRANSFER_MASTER_RESPONSE, AdminHandler.handleTransferMasterResponse);
    dispatcher.registerServer(MessageType.MASTER_CHANGED_NOTIFICATION, AdminHandler.handleMasterChangedNotification);

    dispatcher.registerServer(MessageType.SERVER_NOTIFICATION, SystemHandler.handleServerNotification);
    dispatcher.registerServer(MessageType.PONG, SystemHandler.handlePong);
}