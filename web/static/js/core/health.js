// The engine's /api/health: its version and environment. Asked once per connect and shared by
// every view in this window, since the engine that comes back may be another one.
import { useEffect, useState } from './html.js';
import { api } from './api.js';
import { ws } from './connection.js';

// A failed ask is asked again while the socket stays up: one miss on a Sim engine left the window
// with no bar until the next reconnect.
export const HEALTH_RETRY = { ms: 2000 };

let health = null;
let connects = 0;   // which connect an answer belongs to: one from an earlier engine is dropped
const listeners = new Set();

async function load(connect) {
    try {
        const answer = await api.getHealth();
        if (connect === connects) {
            health = answer;
            listeners.forEach(fn => fn(health));
        }
    } catch {
        // keeps the last one known meanwhile
        if (connect === connects && ws.isConnected()) {
            setTimeout(() => connect === connects && ws.isConnected() && load(connect), HEALTH_RETRY.ms);
        }
    }
}

ws.on('connected', () => load(++connects));
if (ws.isConnected()) {
    load(++connects);
}

// { status, version, environment, production }, or null until the first answer.
export function useHealth() {
    const [value, setValue] = useState(health);
    useEffect(() => {
        listeners.add(setValue);
        setValue(health);   // it may have come between the render and here
        return () => listeners.delete(setValue);
    }, []);
    return value;
}
