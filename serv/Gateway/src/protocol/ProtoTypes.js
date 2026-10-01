import path from "node:path";
import { fileURLToPath } from "node:url";
import protobuf from "protobufjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const PROTO_PATH = path.join(
    __dirname,
    "..",
    "..",
    "..",
    "chat_protocol.proto"
);

const root = await protobuf.load(PROTO_PATH);

export const LoginRequest = root.lookupType("chat.LoginRequest");
export const LoginResponse = root.lookupType("chat.LoginResponse");

export const RegisterRequest = root.lookupType("chat.RegisterRequest");
export const RegisterResponse = root.lookupType("chat.RegisterResponse");

export const ChatMessage = root.lookupType("chat.ChatMessage");
export const ServerNotification = root.lookupType("chat.ServerNotification");

export const CreateRoomRequest = root.lookupType("chat.CreateRoomRequest");
export const CreateRoomResponse = root.lookupType("chat.CreateRoomResponse");

export const RoomListRequest = root.lookupType("chat.RoomListRequest");
export const RoomListResponse = root.lookupType("chat.RoomListResponse");

export const JoinRoomRequest = root.lookupType("chat.JoinRoomRequest");
export const JoinRoomResponse = root.lookupType("chat.JoinRoomResponse");

export const LeaveRoomRequest = root.lookupType("chat.LeaveRoomRequest");
export const LeaveRoomResponse = root.lookupType("chat.LeaveRoomResponse");

export const ChatHistoryRequest = root.lookupType("chat.ChatHistoryRequest");
export const ChatHistoryResponse = root.lookupType("chat.ChatHistoryResponse");

export const WhisperRequest = root.lookupType("chat.WhisperRequest");
export const WhisperResponse = root.lookupType("chat.WhisperResponse");
export const WhisperNotification = root.lookupType("chat.WhisperNotification");

export const KickUserRequest = root.lookupType("chat.KickUserRequest");
export const KickUserResponse = root.lookupType("chat.KickUserResponse");
export const KickedNotification = root.lookupType("chat.KickedNotification");

export const TransferMasterRequest =
    root.lookupType("chat.TransferMasterRequest");

export const TransferMasterResponse =
    root.lookupType("chat.TransferMasterResponse");

export const MasterChangedNotification =
    root.lookupType("chat.MasterChangedNotification");