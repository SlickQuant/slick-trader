/**
 * REST API client for SlickTrader backend
 */
class ApiClient {
    constructor(baseUrl = '') {
        this.baseUrl = baseUrl;
    }

    /**
     * Make HTTP request
     */
    async request(endpoint, options = {}) {
        const url = `${this.baseUrl}${endpoint}`;
        const defaultOptions = {
            headers: {
                'Content-Type': 'application/json',
            },
        };

        const config = { ...defaultOptions, ...options };

        try {
            const response = await fetch(url, config);

            if (!response.ok) {
                const errorData = await response.json().catch(() => ({ error: 'Unknown error' }));
                throw new Error(errorData.error || `HTTP ${response.status}: ${response.statusText}`);
            }

            return await response.json();
        } catch (error) {
            console.error(`[API] Request failed: ${endpoint}`, error);
            throw error;
        }
    }

    /**
     * GET request
     */
    async get(endpoint) {
        return this.request(endpoint, { method: 'GET' });
    }

    /**
     * POST request
     */
    async post(endpoint, data) {
        return this.request(endpoint, {
            method: 'POST',
            body: JSON.stringify(data),
        });
    }

    /**
     * PUT request
     */
    async put(endpoint, data) {
        return this.request(endpoint, {
            method: 'PUT',
            body: JSON.stringify(data),
        });
    }

    /**
     * DELETE request
     */
    async delete(endpoint) {
        return this.request(endpoint, { method: 'DELETE' });
    }

    // ==================== API Endpoints ====================

    /**
     * Get system health status
     */
    async getHealth() {
        return this.get('/api/health');
    }

    /**
     * Get all strategies
     */
    async getStrategies() {
        return this.get('/api/strategies');
    }

    /**
     * Get all positions
     */
    async getAllPositions() {
        return this.get('/api/positions');
    }

    /**
     * Get positions for specific strategy
     */
    async getStrategyPositions(strategyId) {
        return this.get(`/api/positions/${strategyId}`);
    }

    /**
     * Get all P&L data
     */
    async getAllPnL() {
        return this.get('/api/pnl');
    }

    /**
     * Get P&L for specific strategy
     */
    async getStrategyPnL(strategyId) {
        return this.get(`/api/pnl/${strategyId}`);
    }

    // ==================== Helper Methods ====================

    /**
     * Format price value
     */
    formatPrice(value, decimals = 2) {
        if (value === null || value === undefined) return '—';
        return Number(value).toFixed(decimals);
    }

    /**
     * Format quantity value
     */
    formatQuantity(value, decimals = 8) {
        if (value === null || value === undefined) return '—';
        return Number(value).toFixed(decimals);
    }

    /**
     * Format currency value
     */
    formatCurrency(value, decimals = 2) {
        if (value === null || value === undefined) return '$0.00';
        const num = Number(value);
        const sign = num >= 0 ? '' : '-';
        const absValue = Math.abs(num);
        return `${sign}$${absValue.toFixed(decimals)}`;
    }

    /**
     * Format percentage value
     */
    formatPercentage(value, decimals = 2) {
        if (value === null || value === undefined) return '—';
        const num = Number(value);
        const sign = num >= 0 ? '+' : '';
        return `${sign}${num.toFixed(decimals)}%`;
    }

    /**
     * Format timestamp
     */
    formatTimestamp(timestampNs) {
        if (!timestampNs) return '—';
        const date = new Date(timestampNs / 1000000); // Convert nanoseconds to milliseconds
        return date.toLocaleTimeString('en-US', {
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
        });
    }

    /**
     * Format date
     */
    formatDate(timestampNs) {
        if (!timestampNs) return '—';
        const date = new Date(timestampNs / 1000000);
        return date.toLocaleDateString('en-US', {
            month: 'short',
            day: 'numeric',
            year: 'numeric',
        });
    }

    /**
     * Get color class based on value
     */
    getColorClass(value) {
        if (value === null || value === undefined || value === 0) return '';
        return value > 0 ? 'positive' : 'negative';
    }
}

// Create global API client instance
const api = new ApiClient();
