import { MessageType } from "../protocol/MessageType.js";
import { LoginRequest, LoginResponse, RegisterRequest, RegisterResponse } from "../protocol/ProtoTypes.js";
import { decodeProto, protoToObject } from "../protocol/PacketCodec.js";


//=======================================================
// Browser -> Gateway -> C++
//=======================================================

export function handleLogin(connection, message) {
    connection.send(
        MessageType.LOGIN_REQUEST,
        LoginRequest,
        {
            username: message.username,
            password: message.password,
            reconnectToken: connection.reconnectToken
        }
    );
}

export function handleRegister(connection, message) {
    connection.send(
        MessageType.REGISTER_REQUEST,
        RegisterRequest,
        {
            username: message.username,
            password: message.password
        }
    );
}


//=======================================================
// C++ -> Gateway -> Browser
//=======================================================

export function handleLoginResponse(connection, payload) {
    const response = decodeProto(LoginResponse, payload);
    const data = protoToObject(LoginResponse, response);

    if (data.success) {
        connection.userId = data.assignedUserId;
        connection.reconnectToken = data.reconnectToken;
    }

    connection.sendBrowser({
        type: "login_response",
        ...data
    });
}

export function handleRegisterResponse(connection, payload) {
    const response = decodeProto(RegisterResponse, payload);
    const data = protoToObject(RegisterResponse, response);

    connection.sendBrowser({
        type: "register_response",
        ...data
    });
}