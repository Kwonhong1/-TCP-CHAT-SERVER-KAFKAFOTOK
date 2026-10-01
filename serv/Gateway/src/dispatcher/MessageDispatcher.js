export class MessageDispatcher {
    constructor() {
        this.browserHandlers = new Map();
        this.serverHandlers = new Map();
    }

    registerBrowser(type, handler) {
        this.browserHandlers.set(type, handler);
    }

    registerServer(messageType, handler) {
        this.serverHandlers.set(messageType, handler);
    }

    async dispatchBrowser(connection, message) {
        const handler = this.browserHandlers.get(message.type);

        if (!handler) {
            throw new Error(
                `Unknown browser message: ${message.type}`
            );
        }

        await handler(connection, message);
    }

    async dispatchServer(connection, header, payload) {
        const handler = this.serverHandlers.get(
            header.messageType
        );

        if (!handler) {
            console.log(
                `[Gateway] Unhandled server message: ${header.messageType}`
            );
            return;
        }

        await handler(connection, payload, header);
    }
}