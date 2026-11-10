// The system log: the main window keeps it, as a ring, and the Log window shows it.

export const LOG_CAPACITY = 500;

export class LogRing {
    constructor(capacity = LOG_CAPACITY) {
        this.capacity = capacity;
        this.entries = [];
        this.listeners = new Set();
    }

    // Adds an entry and returns it: { time (ms), message, level }.
    add(message, level = 'info') {
        const entry = { time: Date.now(), message, level };
        this.entries.push(entry);
        if (this.entries.length > this.capacity) {
            this.entries.splice(0, this.entries.length - this.capacity);
        }
        for (const fn of this.listeners) {
            fn(entry);
        }
        return entry;
    }

    clear() {
        this.entries = [];
    }

    subscribe(fn) {
        this.listeners.add(fn);
        return () => this.listeners.delete(fn);
    }
}

export function orderLogLevel(data) {
    switch (data.event) {
        case 'reject': return 'error';
        case 'cancel': return 'warning';
        case 'fill':
        case 'leg_fill': return 'success';
        default: return 'info';
    }
}

// One line for an order_update message.
export function formatOrderLog(data) {
    const side = data.side ? data.side.toUpperCase() : '';
    const price = data.price ?? '';
    const qty = data.qty ?? '';
    const prefix = `${data.instrument || ''} Order ${data.order_id || ''}:`;
    switch (data.event) {
        case 'ack':
            return `${prefix} ${side} ${data.status || 'ACK'} ${qty}@${price}`;
        case 'fill': {
            const remaining = data.remaining_qty ?? 0;
            const status = remaining > 0 ? 'PARTIALLY_FILLED' : 'FILLED';
            return `${prefix} ${side} ${status} ${data.fill_qty ?? ''}@${data.fill_price ?? ''}` +
                (remaining > 0 ? ` (${remaining} remaining)` : '');
        }
        case 'leg_fill':
            // One leg of a spread order's fill: the leg's own instrument, side and price.
            return `${prefix} leg ${side} ${data.fill_qty ?? ''}@${data.fill_price ?? ''}`;
        case 'cancel':
            return `${prefix} ${side} CANCELLED ${qty}@${price}`;
        case 'reject':
            return `${prefix} ${side} REJECTED ${qty}@${price}${data.reason ? ` - ${data.reason}` : ''}`;
        default:
            return `${prefix} ${data.event}`;
    }
}

export function formatLogTime(ms) {
    return new Date(ms).toLocaleTimeString('en-US', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
}
