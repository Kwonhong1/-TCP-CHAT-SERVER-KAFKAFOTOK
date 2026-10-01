let socket = null;

const handlers = new Map();

export function connect() {
    const protocol =
        location.protocol === "https:"
            ? "wss:"
            : "ws:";

    const url =
        `${protocol}//${location.hostname}:ws`;

    socket = new WebSocket(url);

    socket.addEventListener("open", () => {
        dispatch({
            type: "gateway_connected"
        });
    });

    socket.addEventListener("message", (event) => {
        try {
            const message = JSON.parse(event.data);
            dispatch(message);
        }
        catch (err) {
            console.error(
                "Invalid Gateway message:",
                err
            );
        }
    });

    socket.addEventListener("close", () => {
        dispatch({
            type: "gateway_disconnected"
        });
    });

    socket.addEventListener("error", () => {
        dispatch({
            type: "gateway_error",
            errorMessage: "WebSocket error"
        });
    });
}

export function send(message) {
    if (
        !socket ||
        socket.readyState !== WebSocket.OPEN
    ) {
        throw new Error("Gateway is not connected");
    }

    socket.send(
        JSON.stringify(message)
    );
}

export function on(type, handler) {
    if (!handlers.has(type)) {
        handlers.set(type, []);
    }

    handlers.get(type).push(handler);
}

function dispatch(message) {
    const list = handlers.get(message.type);

    if (!list) {
        console.log(
            "Unhandled Gateway message:",
            message
        );

        return;
    }

    for (const handler of list) {
        handler(message);
    }
}