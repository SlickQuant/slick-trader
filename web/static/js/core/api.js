// REST client for the SlickTrader backend, and the formatting every view shares.
class ApiClient {
    constructor(baseUrl = '') {
        this.baseUrl = baseUrl;
    }

    async request(endpoint, options = {}) {
        const response = await fetch(`${this.baseUrl}${endpoint}`, {
            headers: { 'Content-Type': 'application/json' },
            ...options,
        });
        if (!response.ok) {
            const error = await response.json().catch(() => ({ error: 'Unknown error' }));
            throw new Error(error.error || `HTTP ${response.status}: ${response.statusText}`);
        }
        return response.json();
    }

    get(endpoint) { return this.request(endpoint, { method: 'GET' }); }
    post(endpoint, data) { return this.request(endpoint, { method: 'POST', body: JSON.stringify(data) }); }
    put(endpoint, data) { return this.request(endpoint, { method: 'PUT', body: JSON.stringify(data) }); }

    getHealth() { return this.get('/api/health'); }
    getBrokers() { return this.get('/api/brokers'); }
    getStrategies() { return this.get('/api/strategies'); }
    getAllPositions() { return this.get('/api/positions'); }
    getAllPnL() { return this.get('/api/pnl'); }
    getOpenOrders() { return this.get('/api/orders'); }
    getFills() { return this.get('/api/fills'); }
    submitOrder(order) { return this.post('/api/orders', order); }
    controlStrategy(strategyId, action) {
        return this.post(`/api/strategies/${encodeURIComponent(strategyId)}/control`, { action });
    }
    getStrategyConfig(strategyId) { return this.get(`/api/strategies/${encodeURIComponent(strategyId)}/config`); }
    putStrategyConfig(strategyId, params) { return this.put(`/api/strategies/${encodeURIComponent(strategyId)}/config`, params); }
    getLadderConfig() { return this.get('/api/ladder-config'); }
    saveLadderConfig(config) { return this.put('/api/ladder-config', config); }

    // order_id is the broker's; cancel_order() needs the client_order_id too.
    cancelOrder(strategyId, orderId, clientOrderId) {
        return this.request(`/api/orders/${encodeURIComponent(strategyId)}/${encodeURIComponent(orderId)}`, {
            method: 'DELETE',
            body: JSON.stringify({ client_order_id: clientOrderId }),
        });
    }
    // updates: { client_order_id, new_price?, new_quantity? }
    modifyOrder(strategyId, orderId, updates) {
        return this.put(`/api/orders/${encodeURIComponent(strategyId)}/${encodeURIComponent(orderId)}`, updates);
    }
    cancelAllOrders() { return this.request('/api/orders/cancel-all', { method: 'DELETE', body: '{}' }); }
}

export const api = new ApiClient();

export function formatQuantity(value, decimals = 8) {
    return value === null || value === undefined ? '—' : Number(value).toFixed(decimals);
}

export function formatCurrency(value, decimals = 2) {
    if (value === null || value === undefined) return '$0.00';
    const num = Number(value);
    return `${num < 0 ? '-' : ''}$${Math.abs(num).toFixed(decimals)}`;
}

// The price as sent, with no rounding a tick finer than two decimals would lose.
export function formatPrice(value) {
    return value === null || value === undefined ? '—' : String(value);
}

// HH:MM:SS.mmm of a nanosecond timestamp.
export function formatTime(timestampNs) {
    if (!timestampNs) return '—';
    const date = new Date(Number(timestampNs) / 1e6);
    const ms = String(date.getMilliseconds()).padStart(3, '0');
    return `${date.toLocaleTimeString('en-US', { hour12: false })}.${ms}`;
}

export function colorClass(value) {
    if (!value) return '';
    return value > 0 ? 'positive' : 'negative';
}
