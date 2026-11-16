// Messages between one dashboard's windows: { kind, dashboard, ...data }. A dashboard is a main
// window and the tool windows it opened: two open on one host keep apart. A window does not get its
// own messages.
const channel = typeof BroadcastChannel === 'undefined' ? null : new BroadcastChannel('slick-trader');
const handlers = new Map();   // kind -> Set of functions

let own = null;   // a main window's dashboard
let known;        // a tool window's, as its main window last said

// The dashboard this window is in. A tool window asks the main window that opened it each time,
// since a reload gives that one a new id. Once it can't, as it closes, the last id stands.
export function dashboard() {
    if (own) {
        return own;
    }
    try {
        known = window.opener?.stDashboard ?? known;
    } catch {
        // an opener on another origin
    }
    return known;
}

// Makes this main window a dashboard of its own. A duplicated tab gets another.
export function startDashboard() {
    own = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
    window.stDashboard = own;
}

if (channel) {
    channel.onmessage = (event) => {
        const message = event.data;
        if (!message?.dashboard || message.dashboard !== dashboard()) {
            return;
        }
        for (const fn of handlers.get(message.kind) ?? []) {
            fn(message);
        }
    };
}

export function post(kind, data = {}) {
    channel?.postMessage({ kind, ...data, dashboard: dashboard() });
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
