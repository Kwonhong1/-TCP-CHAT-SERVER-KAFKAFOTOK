import { send, on } from "./socket.js";

let loginSuccessHandler = null;

export function initAuth(onLoginSuccess) {
    loginSuccessHandler = onLoginSuccess;

    document
        .getElementById("login-button")
        .addEventListener("click", login);

    document
        .getElementById("register-button")
        .addEventListener("click", register);

    on("login_response", handleLoginResponse);
    on("register_response", handleRegisterResponse);
}

function login() {
    const username = document.getElementById("login-username").value.trim();
    const password = document.getElementById("login-password").value;

    if (!username || !password) {
        showMessage("아이디와 비밀번호를 입력하세요.");
        return;
    }

    send({
        type: "login",
        username,
        password
    });
}

function register() {
    const username = document.getElementById("register-username").value.trim();
    const password = document.getElementById("register-password").value;

    if (!username || !password) {
        showMessage("아이디와 비밀번호를 입력하세요.");
        return;
    }

    send({
        type: "register",
        username,
        password
    });
}

function handleLoginResponse(message) {
    if (!message.success) {
        showMessage(
            message.errorMessage ||
            "로그인에 실패했습니다."
        );

        return;
    }

    showMessage("");

    if (loginSuccessHandler) {
        loginSuccessHandler(message);
    }
}

function handleRegisterResponse(message) {
    if (!message.success) {
        showMessage(
            message.errorMessage ||
            "회원가입에 실패했습니다."
        );

        return;
    }

    showMessage("회원가입 성공. 로그인하세요.");
}

function showMessage(message) {
    document.getElementById("auth-message").textContent = message;
}