// Messages between this host's windows: { kind, ...data }. A window does not get its own.
const channel = typeof BroadcastChannel === 'undefined' ? null : new BroadcastChannel('slick-trader');
const handlers = new Map();   // kind -> Set of functions

if (channel) {
    channel.onmessage = (event) => {
        const message = event.data;
        for (const fn of handlers.get(message?.kind) ?? []) {
            fn(message);
        }
    };
}

export function post(kind, data = {}) {
    channel?.postMessage({ kind, ...data });
}

// Calls fn(message) for each message of `kind`; returns what stops it.
export function on(kind, fn) {
    if (!handlers.has(kind)) {
        handlers.set(kind, new Set());
    }
    handlers.get(kind).add(fn);
    return () => handlers.get(kind)?.delete(fn);
}

// An entry for the main window's log, from what a user did in this one.
export function log(message, level = 'info') {
    post('log', { message, level });
}
