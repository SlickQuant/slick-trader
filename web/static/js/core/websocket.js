// WebSocket client with reconnect: each message is emitted by its "type", as are 'connected',
// 'disconnected', 'reconnecting' and 'error' for the socket itself.
export class WebSocketClient {
    constructor(url) {
        this.url = url;
        this.ws = null;
        this.reconnectAttempts = 0;
        // A trading window stays open for the session: it keeps trying through an engine restart.
        this.maxReconnectAttempts = Infinity;
        this.reconnectDelay = 1000;
        this.maxReconnectDelay = 30000;
        this.listeners = {};
        this.isConnecting = false;
        this.intentionallyClosed = false;
        this.pingInterval = null;
    }

    connect() {
        if (this.isConnecting || (this.ws && this.ws.readyState === WebSocket.OPEN)) {
            return;
        }
        this.isConnecting = true;
        this.intentionallyClosed = false;

        try {
            this.ws = new WebSocket(this.url);
            this.ws.onopen = (event) => {
                this.isConnecting = false;
                this.reconnectAttempts = 0;
                this.emit('connected', event);
                this.startPingInterval();
            };
            this.ws.onmessage = (event) => {
                let data;
                try {
                    data = JSON.parse(event.data);
                } catch (error) {
                    console.error('[WebSocket] Failed to parse message:', error);
                    return;
                }
                this.emit('message', data);
                if (data.type) {
                    this.emit(data.type, data);
                }
            };
            this.ws.onerror = (error) => {
                this.isConnecting = false;
                this.emit('error', error);
            };
            this.ws.onclose = (event) => {
                this.isConnecting = false;
                this.stopPingInterval();
                this.emit('disconnected', event);
                if (!this.intentionallyClosed && this.reconnectAttempts < this.maxReconnectAttempts) {
                    this.scheduleReconnect();
                }
            };
        } catch (error) {
            this.isConnecting = false;
            this.emit('error', error);
            this.scheduleReconnect();
        }
    }

    // Exponential backoff, capped.
    scheduleReconnect() {
        this.reconnectAttempts++;
        const delay = Math.min(this.reconnectDelay * Math.pow(1.5, this.reconnectAttempts), this.maxReconnectDelay);
        this.emit('reconnecting', { attempt: this.reconnectAttempts, delay });
        setTimeout(() => this.connect(), delay);
    }

    send(data) {
        if (this.isConnected()) {
            this.ws.send(typeof data === 'string' ? data : JSON.stringify(data));
        }
    }

    startPingInterval() {
        this.stopPingInterval();
        this.pingInterval = setInterval(() => this.send({ type: 'ping' }), 30000);
    }

    stopPingInterval() {
        if (this.pingInterval) {
            clearInterval(this.pingInterval);
            this.pingInterval = null;
        }
    }

    close() {
        this.intentionallyClosed = true;
        this.stopPingInterval();
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    on(event, callback) {
        (this.listeners[event] ??= []).push(callback);
    }

    off(event, callback) {
        if (this.listeners[event]) {
            this.listeners[event] = this.listeners[event].filter(cb => cb !== callback);
        }
    }

    emit(event, data) {
        for (const callback of this.listeners[event] ?? []) {
            try {
                callback(data);
            } catch (error) {
                console.error(`[WebSocket] Error in event handler for ${event}:`, error);
            }
        }
    }

    isConnected() {
        return !!this.ws && this.ws.readyState === WebSocket.OPEN;
    }
}
