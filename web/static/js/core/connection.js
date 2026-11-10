// This window's WebSocket to the engine, one per window, and the hooks views read it with.
import { WebSocketClient } from './websocket.js';
import { useEffect, useRef, useState } from './html.js';

const url = `${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}`;
export const ws = new WebSocketClient(url);

// Channels this window asked for, asked for again on every connect. The instruments list goes
// only to a window that asks: it runs to megabytes.
const channels = new Set();
ws.on('connected', () => {
    for (const channel of channels) {
        ws.send({ type: 'subscribe', channel });
    }
});

export function subscribeChannel(channel) {
    if (!channels.has(channel)) {
        channels.add(channel);
        ws.send({ type: 'subscribe', channel });   // dropped unless connected; the next connect sends it
    }
}

export function connect() {
    ws.connect();
}

// Calls fn(data) for each message of `type` while the component is mounted.
export function useWs(type, fn) {
    const latest = useRef(fn);
    latest.current = fn;
    useEffect(() => {
        const handler = (data) => latest.current(data);
        ws.on(type, handler);
        return () => ws.off(type, handler);
    }, [type]);
}

// Calls fn() now if connected, and on every connect after: a view reloads its snapshot there.
export function useOnConnect(fn) {
    const latest = useRef(fn);
    latest.current = fn;
    useEffect(() => {
        const handler = () => latest.current();
        ws.on('connected', handler);
        if (ws.isConnected()) {
            handler();
        }
        return () => ws.off('connected', handler);
    }, []);
}

// 'connecting' | 'connected' | 'disconnected'
export function useConnection() {
    const [state, setState] = useState(ws.isConnected() ? 'connected' : 'connecting');
    useWs('connected', () => setState('connected'));
    useWs('disconnected', () => setState('disconnected'));
    useWs('reconnecting', () => setState('connecting'));
    return state;
}
