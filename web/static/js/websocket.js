/**
 * WebSocket client for real-time data streaming
 */
class WebSocketClient {
    constructor(url) {
        this.url = url;
        this.ws = null;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 10;
        this.reconnectDelay = 1000;
        this.maxReconnectDelay = 30000;
        this.listeners = {};
        this.isConnecting = false;
        this.intentionallyClosed = false;
        this.pingInterval = null;
    }

    /**
     * Connect to WebSocket server
     */
    connect() {
        if (this.isConnecting || (this.ws && this.ws.readyState === WebSocket.OPEN)) {
            return;
        }

        this.isConnecting = true;
        this.intentionallyClosed = false;

        try {
            console.log(`[WebSocket] Connecting to ${this.url}`);
            this.ws = new WebSocket(this.url);

            this.ws.onopen = (event) => {
                console.log('[WebSocket] Connected');
                this.isConnecting = false;
                this.reconnectAttempts = 0;
                this.reconnectDelay = 1000;
                this.emit('connected', event);
                this.startPingInterval();
            };

            this.ws.onmessage = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    // console.log('[WebSocket] Message received:', data.type);
                    this.emit('message', data);

                    // Emit specific event types
                    if (data.type) {
                        this.emit(data.type, data);
                    }
                } catch (error) {
                    console.error('[WebSocket] Failed to parse message:', error);
                }
            };

            this.ws.onerror = (error) => {
                console.error('[WebSocket] Error:', error);
                this.isConnecting = false;
                this.emit('error', error);
            };

            this.ws.onclose = (event) => {
                console.log('[WebSocket] Disconnected');
                this.isConnecting = false;
                this.stopPingInterval();
                this.emit('disconnected', event);

                // Attempt reconnection if not intentionally closed
                if (!this.intentionallyClosed && this.reconnectAttempts < this.maxReconnectAttempts) {
                    this.scheduleReconnect();
                }
            };

        } catch (error) {
            console.error('[WebSocket] Connection error:', error);
            this.isConnecting = false;
            this.emit('error', error);
            this.scheduleReconnect();
        }
    }

    /**
     * Schedule reconnection with exponential backoff
     */
    scheduleReconnect() {
        this.reconnectAttempts++;
        const delay = Math.min(
            this.reconnectDelay * Math.pow(1.5, this.reconnectAttempts),
            this.maxReconnectDelay
        );

        console.log(`[WebSocket] Reconnecting in ${delay}ms (attempt ${this.reconnectAttempts})`);
        this.emit('reconnecting', { attempt: this.reconnectAttempts, delay });

        setTimeout(() => {
            this.connect();
        }, delay);
    }

    /**
     * Send message to server
     */
    send(data) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            const message = typeof data === 'string' ? data : JSON.stringify(data);
            this.ws.send(message);
            // console.log('[WebSocket] Sent:', data);
        } else {
            console.warn('[WebSocket] Cannot send, not connected');
        }
    }

    /**
     * Subscribe to data streams
     */
    subscribe(channels) {
        this.send({
            type: 'subscribe',
            channels: channels
        });
    }

    /**
     * Start ping interval to keep connection alive
     */
    startPingInterval() {
        this.stopPingInterval();
        this.pingInterval = setInterval(() => {
            this.send({ type: 'ping' });
        }, 30000); // Ping every 30 seconds
    }

    /**
     * Stop ping interval
     */
    stopPingInterval() {
        if (this.pingInterval) {
            clearInterval(this.pingInterval);
            this.pingInterval = null;
        }
    }

    /**
     * Close WebSocket connection
     */
    close() {
        this.intentionallyClosed = true;
        this.stopPingInterval();

        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    /**
     * Add event listener
     */
    on(event, callback) {
        if (!this.listeners[event]) {
            this.listeners[event] = [];
        }
        this.listeners[event].push(callback);
    }

    /**
     * Remove event listener
     */
    off(event, callback) {
        if (this.listeners[event]) {
            this.listeners[event] = this.listeners[event].filter(cb => cb !== callback);
        }
    }

    /**
     * Emit event to all listeners
     */
    emit(event, data) {
        if (this.listeners[event]) {
            this.listeners[event].forEach(callback => {
                try {
                    callback(data);
                } catch (error) {
                    console.error(`[WebSocket] Error in event handler for ${event}:`, error);
                }
            });
        }
    }

    /**
     * Get connection state
     */
    getState() {
        if (!this.ws) return 'DISCONNECTED';

        switch (this.ws.readyState) {
            case WebSocket.CONNECTING:
                return 'CONNECTING';
            case WebSocket.OPEN:
                return 'CONNECTED';
            case WebSocket.CLOSING:
                return 'CLOSING';
            case WebSocket.CLOSED:
                return 'DISCONNECTED';
            default:
                return 'UNKNOWN';
        }
    }

    /**
     * Check if connected
     */
    isConnected() {
        return this.ws && this.ws.readyState === WebSocket.OPEN;
    }
}
