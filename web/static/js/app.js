/**
 * Main dashboard application
 */
class Dashboard {
    constructor() {
        this.wsClient = null;
        this.refreshInterval = null;
        this.pnlHistory = [];
        this.lastPnLUpdate = null;
        this.isInitialized = false;
        this.strategies = [];  // Store loaded strategies
    }

    /**
     * Initialize the dashboard
     */
    async init() {
        console.log('[Dashboard] Initializing...');

        // Initialize chart
        chartManager.initPnLChart('pnlChart');

        // Setup event listeners
        this.setupEventListeners();

        // Connect to WebSocket
        this.connectWebSocket();

        // Initial data load
        await this.loadInitialData();

        // Start periodic refresh
        this.startPeriodicRefresh();

        this.isInitialized = true;
        this.log('Dashboard initialized', 'success');
        console.log('[Dashboard] Initialized successfully');
    }

    /**
     * Setup DOM event listeners
     */
    setupEventListeners() {
        // Refresh buttons
        document.getElementById('refreshPositions')?.addEventListener('click', () => {
            this.loadPositions();
        });

        document.getElementById('refreshStrategies')?.addEventListener('click', () => {
            this.loadStrategies();
            this.loadPnL();
        });

        // Clear log button
        document.getElementById('clearLog')?.addEventListener('click', () => {
            this.clearLog();
        });

        // Orders panel buttons
        document.getElementById('refreshOrders')?.addEventListener('click', () => {
            this.loadOrders();
        });

        document.getElementById('cancelAllOrders')?.addEventListener('click', () => {
            this.confirmCancelAllOrders();
        });

        // Chart timeframe buttons
        document.querySelectorAll('.chart-controls .btn').forEach(btn => {
            btn.addEventListener('click', (e) => {
                document.querySelectorAll('.chart-controls .btn').forEach(b => b.classList.remove('active'));
                e.target.classList.add('active');
                // TODO: Implement timeframe filtering
                this.log(`Chart timeframe changed to ${e.target.dataset.timeframe}`, 'info');
            });
        });
    }

    /**
     * Connect to WebSocket server
     */
    connectWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}`;

        this.wsClient = new WebSocketClient(wsUrl);

        // Connection status handlers
        this.wsClient.on('connected', () => {
            this.updateConnectionStatus('connected');
            this.log('WebSocket connected', 'success');

            // Clear stale order book data so the ladder shows fresh data from the new connection
            if (window.ladderManager) {
                window.ladderManager.clearBooks();
            }

            // Subscribe to level2 for instruments (if ladder already initialized)
            this.subscribeToMarketData();
        });

        this.wsClient.on('disconnected', () => {
            this.updateConnectionStatus('disconnected');
            this.log('WebSocket disconnected', 'error');
        });

        this.wsClient.on('reconnecting', (data) => {
            this.updateConnectionStatus('connecting');
            this.log(`Reconnecting (attempt ${data.attempt})...`, 'warning');
        });

        this.wsClient.on('error', (error) => {
            this.log('WebSocket error', 'error');
            console.error('[Dashboard] WebSocket error:', error);
        });

        // Message handlers
        this.wsClient.on('welcome', (data) => {
            this.log(data.message, 'info');
        });

        this.wsClient.on('pong', () => {
            // console.log('[Dashboard] Received pong');
        });

        // Real-time data handlers
        this.wsClient.on('pnl_update', (data) => {
            this.handlePnLUpdate(data);
        });

        this.wsClient.on('position_update', (data) => {
            this.handlePositionUpdate(data);
        });

        this.wsClient.on('order_update', (data) => {
            this.handleOrderUpdate(data);
        });

        // level2 update handler
        this.wsClient.on('level2', (data) => {
            this.handleLevel2Update(data);
        });

        // Broker status update handler
        this.wsClient.on('broker_status', (data) => {
            this.renderBrokerStatus(data.brokers);
        });

        // Connect
        this.wsClient.connect();
    }

    /**
     * Load initial data
     */
    async loadInitialData() {
        try {
            // Check health
            const health = await api.getHealth();
            console.log('[Dashboard] Health check:', health);

            // Load all data in parallel
            await Promise.all([
                this.loadBrokers(),
                this.loadPnL(),
                this.loadPositions(),
                this.loadStrategies(),
                this.loadOrders()
            ]);

        } catch (error) {
            console.error('[Dashboard] Failed to load initial data:', error);
            this.log('Failed to load initial data: ' + error.message, 'error');
        }
    }

    /**
     * Load and render broker status
     */
    async loadBrokers() {
        try {
            const data = await api.getBrokers();
            this.renderBrokerStatus(data.brokers || []);
        } catch (error) {
            console.error('[Dashboard] Failed to load brokers:', error);
            const el = document.getElementById('brokerStatusList');
            if (el) el.innerHTML = '<span class="broker-status-loading">Unavailable</span>';
        }
    }

    renderBrokerStatus(brokers) {
        const el = document.getElementById('brokerStatusList');
        if (!el) return;
        if (!brokers || brokers.length === 0) {
            el.innerHTML = '<span class="broker-status-loading">No brokers configured</span>';
            return;
        }
        el.innerHTML = brokers.map(b => {
            const mdDot = !b.md.enabled ? 'dot-disabled' : b.md.connected ? 'dot-connected' : 'dot-disconnected';
            const ordDot = !b.order.enabled ? 'dot-disabled' : b.order.connected ? 'dot-connected' : 'dot-disconnected';
            const mdLabel = !b.md.enabled ? 'MD (off)' : b.md.connected ? 'MD' : 'MD (disconnected)';
            const ordLabel = !b.order.enabled ? 'Order (off)' : b.order.connected ? 'Order' : 'Order (disconnected)';
            return `
                <div class="broker-item">
                    <span class="broker-name">${b.name}</span>
                    <div class="broker-connectors">
                        <span class="connector-badge"><span class="dot ${mdDot}"></span>${mdLabel}</span>
                        <span class="connector-badge"><span class="dot ${ordDot}"></span>${ordLabel}</span>
                    </div>
                </div>`;
        }).join('');
    }

    /**
     * Load P&L data
     */
    async loadPnL() {
        try {
            const pnlData = await api.getAllPnL();
            this.updatePnLDisplay(pnlData);

            // Merge with strategies list for complete table
            await this.updatePnLTable(pnlData);
        } catch (error) {
            console.error('[Dashboard] Failed to load P&L:', error);
        }
    }

    /**
     * Load positions
     */
    async loadPositions() {
        try {
            const positions = await api.getAllPositions();
            this.updatePositionsTable(positions);
        } catch (error) {
            console.error('[Dashboard] Failed to load positions:', error);
        }
    }

    /**
     * Load strategies
     */
    async loadStrategies() {
        try {
            const strategies = await api.getStrategies();
            this.updateStrategiesCount(strategies);
            this.strategies = strategies;  // Store for later use

            // Initialize ladders with strategy instruments
            if (typeof ladderManager !== 'undefined') {
                await this.initializeLadders(strategies);
            }
        } catch (error) {
            console.error('[Dashboard] Failed to load strategies:', error);
        }
    }

    /**
     * Initialize price ladders from strategies
     */
    async initializeLadders(strategies) {
        try {
            if (strategies && strategies.length > 0) {
                await ladderManager.init(strategies);
                this.log('Price ladders initialized', 'success');

                // Subscribe to market data now that instruments are known
                this.subscribeToMarketData();
            } else {
                console.warn('[Dashboard] No strategies found for ladder initialization');
            }
        } catch (error) {
            console.error('[Dashboard] Failed to initialize ladders:', error);
            this.log('Failed to initialize ladders: ' + error.message, 'error');
        }
    }

    /**
     * Control strategy (start/stop/pause/resume)
     */
    async controlStrategy(strategyId, action) {
        try {
            const result = await api.controlStrategy(strategyId, action);
            this.log(`Strategy ${strategyId}: ${action}`, 'success');
            modalManager.showNotification(`Strategy ${action} successful`, 'success');

            // Reload strategies
            await this.loadStrategies();
        } catch (error) {
            this.log(`Strategy control failed: ${error.message}`, 'error');
            modalManager.showNotification(`Failed to ${action} strategy`, 'error');
        }
    }

    /**
     * Update P&L summary display
     */
    updatePnLDisplay(pnlData) {
        let totalPnL = 0;
        let totalRealized = 0;
        let totalUnrealized = 0;

        // Aggregate P&L from all strategies (realized and unrealized are already net of fees)
        Object.values(pnlData).forEach(strategyPnl => {
            totalPnL += strategyPnl.total_pnl || 0;
            totalRealized += strategyPnl.realized_pnl || 0;
            totalUnrealized += strategyPnl.unrealized_pnl || 0;
        });

        // Update summary cards
        const totalPnlEl = document.getElementById('totalPnl');
        const realizedPnlEl = document.getElementById('realizedPnl');
        const unrealizedPnlEl = document.getElementById('unrealizedPnl');

        if (totalPnlEl) {
            totalPnlEl.textContent = api.formatCurrency(totalPnL);
            totalPnlEl.className = 'metric-value ' + api.getColorClass(totalPnL);
        }

        if (realizedPnlEl) {
            realizedPnlEl.textContent = api.formatCurrency(totalRealized);
            realizedPnlEl.className = 'metric-value ' + api.getColorClass(totalRealized);
        }

        if (unrealizedPnlEl) {
            unrealizedPnlEl.textContent = api.formatCurrency(totalUnrealized);
            unrealizedPnlEl.className = 'metric-value ' + api.getColorClass(totalUnrealized);
        }

        // Update chart
        const timestamp = Date.now();
        chartManager.addPnLDataPoint(timestamp, totalPnL, totalRealized, totalUnrealized);
        this.lastPnLUpdate = totalPnL;
    }

    /**
     * Update strategy P&L table
     */
    async updatePnLTable(pnlData) {
        const tbody = document.getElementById('strategiesTableBody');
        if (!tbody) return;

        // Get strategies list if not already loaded
        if (!this.strategies || this.strategies.length === 0) {
            try {
                this.strategies = await api.getStrategies();
            } catch (error) {
                console.error('[Dashboard] Failed to load strategies for table:', error);
                this.strategies = [];
            }
        }

        // If no strategies at all, show empty state
        if (!this.strategies || this.strategies.length === 0) {
            tbody.innerHTML = '<tr class="empty-state"><td colspan="6">No strategies loaded</td></tr>';
            return;
        }

        // Build table rows for all strategies
        tbody.innerHTML = this.strategies.map(strategy => {
            const strategyId = strategy.name || strategy.id;
            const pnl = pnlData[strategyId] || {
                realized_pnl: 0,
                unrealized_pnl: 0,
                total_pnl: 0
            };
            const isRunning = strategy.status === 'running';
            const isHalted = strategy.status === 'halted';
            const isPaused = strategy.status === 'paused';
            const statusLabel = isHalted || isPaused
                ? (strategy.halt_reason || strategy.status).split(':')[0].trim()
                : (strategy.status || 'unknown');
            const statusTitle = (isHalted || isPaused) ? (strategy.halt_reason || '') : '';

            return `
                <tr>
                    <td>${strategyId}</td>
                    <td><span class="status-badge status-${strategy.status || 'unknown'}" title="${statusTitle}">${statusLabel}</span></td>
                    <td class="${api.getColorClass(pnl.realized_pnl)}">${api.formatCurrency(pnl.realized_pnl)}</td>
                    <td class="${api.getColorClass(pnl.unrealized_pnl)}">${api.formatCurrency(pnl.unrealized_pnl)}</td>
                    <td class="${api.getColorClass(pnl.total_pnl)}">${api.formatCurrency(pnl.total_pnl)}</td>
                    <td>
                        <div class="action-buttons">
                            <button class="btn btn-sm btn-success" onclick="window.dashboard.controlStrategy('${strategyId}', 'start')" title="Start" ${isRunning ? 'disabled' : ''}>▶</button>
                            <button class="btn btn-sm btn-danger" onclick="window.dashboard.controlStrategy('${strategyId}', 'stop')" title="Stop">⏹</button>
                            <button class="btn btn-sm" onclick="modalManager.showOrderModal('${strategyId}', 'BTC-USD')" title="New Order">📝</button>
                        </div>
                    </td>
                </tr>
            `;
        }).join('');
    }

    /**
     * Update positions table
     */
    updatePositionsTable(positionsData) {
        const tbody = document.getElementById('positionsTableBody');
        if (!tbody) return;

        let rows = [];

        Object.entries(positionsData).forEach(([strategyId, positions]) => {
            positions.forEach(position => {
                rows.push({
                    strategy: strategyId,
                    instrument: position.instrument,
                    quantity: position.quantity,
                    avgPrice: position.avg_price,
                    realizedPnl: position.realized_pnl
                });
            });
        });

        if (rows.length === 0) {
            tbody.innerHTML = '<tr class="empty-state"><td colspan="5">No positions</td></tr>';
            return;
        }

        tbody.innerHTML = rows.map(pos => `
            <tr>
                <td>${pos.strategy}</td>
                <td>${pos.instrument}</td>
                <td class="${api.getColorClass(pos.quantity)}">${api.formatQuantity(pos.quantity, 4)}</td>
                <td>${api.formatCurrency(pos.avgPrice)}</td>
                <td class="${api.getColorClass(pos.realizedPnl)}">${api.formatCurrency(pos.realizedPnl)}</td>
            </tr>
        `).join('');
    }

    /**
     * Update strategies count
     */
    updateStrategiesCount(strategies) {
        const listEl = document.getElementById('activeStrategies');
        if (!listEl) return;
        const items = Array.isArray(strategies) ? strategies : Object.values(strategies);
        if (items.length === 0) {
            listEl.innerHTML = '<div class="strategy-item-empty">No strategies</div>';
            return;
        }
        listEl.innerHTML = items.map(s => {
            const isHalted = s.status === 'halted';
            const statusLabel = isHalted
                ? (s.halt_reason || 'Halted').split(':')[0].trim()
                : (s.status || 'unknown');
            const statusTitle = isHalted ? (s.halt_reason || '') : '';
            return `<div class="strategy-item">` +
                `<span class="strategy-name">${s.name}</span>` +
                `<div class="strategy-item-actions">` +
                `<span class="status-badge status-${s.status}" title="${statusTitle}">${statusLabel}</span>` +
                `<button class="btn-icon" title="Configure" data-strategy="${s.name}">&#9881;</button>` +
                `</div>` +
                `</div>`;
        }).join('');

        // Wire gear button clicks via event delegation (re-registered each render)
        listEl.onclick = (e) => {
            const btn = e.target.closest('[data-strategy]');
            if (btn) this.openStrategyConfig(btn.dataset.strategy);
        };
    }

    async openStrategyConfig(strategyId) {
        try {
            const params = await api.get(`/api/strategies/${encodeURIComponent(strategyId)}/config`);
            document.getElementById('strategyConfigTitle').textContent = `Configure: ${strategyId}`;
            document.getElementById('strategyConfigEditor').value = JSON.stringify(params, null, 2);
            document.getElementById('strategyConfigError').style.display = 'none';
            this._configStrategyId = strategyId;

            const modal = document.getElementById('strategyConfigModal');
            modal.style.display = 'flex';

            // Wire buttons (safe to re-wire each open)
            document.getElementById('strategyConfigClose').onclick  = () => this.closeStrategyConfig();
            document.getElementById('strategyConfigCancel').onclick = () => this.closeStrategyConfig();
            document.getElementById('strategyConfigSave').onclick   = () => this.saveStrategyConfig();
            modal.onclick = (e) => { if (e.target === modal) this.closeStrategyConfig(); };
        } catch (err) {
            console.error('[Dashboard] Failed to load strategy config:', err);
            this.log(`Failed to load config for ${strategyId}`, 'error');
        }
    }

    closeStrategyConfig() {
        document.getElementById('strategyConfigModal').style.display = 'none';
        this._configStrategyId = null;
    }

    async saveStrategyConfig() {
        const errorEl = document.getElementById('strategyConfigError');
        const raw = document.getElementById('strategyConfigEditor').value;

        let params;
        try {
            params = JSON.parse(raw);
        } catch (e) {
            errorEl.textContent = 'Invalid JSON: ' + e.message;
            errorEl.style.display = 'block';
            return;
        }

        try {
            await api.put(`/api/strategies/${encodeURIComponent(this._configStrategyId)}/config`, params);
            this.closeStrategyConfig();
            this.log(`Config updated for ${this._configStrategyId}`, 'success');
        } catch (err) {
            errorEl.textContent = err.message || 'Save failed';
            errorEl.style.display = 'block';
        }
    }

    /**
     * Handle real-time P&L update
     */
    handlePnLUpdate(data) {
        // console.log('[Dashboard] P&L update received:', data);
        this.updatePnLDisplay(data.pnl || data);
    }

    /**
     * Handle real-time position update
     */
    handlePositionUpdate(data) {
        console.log('[Dashboard] Position update received:', data);
        this.loadPositions(); // Reload all positions for simplicity
    }

    /**
     * Handle real-time order update
     */
    handleOrderUpdate(data) {
        console.log('[Dashboard] Order update received:', data);
        const level = data.event === 'reject' ? 'error'
                    : data.event === 'cancel'  ? 'warning'
                    : data.event === 'fill' || data.event === 'leg_fill' ? 'success'
                    : 'info';
        this.log(this._formatOrderLog(data), level);
        if (typeof ladderManager !== 'undefined') {
            ladderManager.onOrderUpdate(data);
        }
        this.loadOrders();
    }

    _formatOrderLog(data) {
        const sym   = data.instrument || '';
        const oid   = data.order_id   || '';
        const side  = data.side ? data.side.toUpperCase() : '';
        const price = data.price  != null ? data.price  : '';
        const qty   = data.qty    != null ? data.qty    : '';
        const prefix = `${sym} Order ${oid}:`;

        switch (data.event) {
            case 'ack':
                return `${prefix} ${side} ${data.status || 'ACK'} ${qty}@${price}`;
            case 'fill': {
                const fp  = data.fill_price    != null ? data.fill_price    : '';
                const fq  = data.fill_qty      != null ? data.fill_qty      : '';
                const rem = data.remaining_qty != null ? data.remaining_qty : 0;
                const st  = rem > 0 ? 'PARTIALLY_FILLED' : 'FILLED';
                return `${prefix} ${side} ${st} ${fq}@${fp}` + (rem > 0 ? ` (${rem} remaining)` : '');
            }
            case 'leg_fill': {
                // One leg of a spread order's fill: the leg's own instrument, side and price.
                const fp = data.fill_price != null ? data.fill_price : '';
                const fq = data.fill_qty   != null ? data.fill_qty   : '';
                return `${prefix} leg ${side} ${fq}@${fp}`;
            }
            case 'cancel':
                return `${prefix} ${side} CANCELLED ${qty}@${price}`;
            case 'reject': {
                const reason = data.reason ? ` - ${data.reason}` : '';
                return `${prefix} ${side} REJECTED ${qty}@${price}${reason}`;
            }
            default:
                return `${prefix} ${data.event}`;
        }
    }

    /**
     * Handle real-time level2 update
     */
    handleLevel2Update(data) {
        if (!data || !data.instrument) {
            console.warn('[Dashboard] Invalid level2 update:', data);
            return;
        }

        // Forward to ladder manager
        if (typeof ladderManager !== 'undefined') {
            ladderManager.updateOrderbookWithLevelUpdate(data.instrument, data);
        }
    }

    /**
     * Handle real-time orderbook update
     */
    handleOrderbookUpdate(data) {
        if (!data || !data.instrument) {
            console.warn('[Dashboard] Invalid orderbook update:', data);
            return;
        }

        // Forward to ladder manager
        if (typeof ladderManager !== 'undefined') {
            ladderManager.updateOrderbook(data.instrument, data);
        }
    }

    /**
     * Get list of instruments to subscribe to from ladder manager
     */
    getSubscribedInstruments() {
        if (typeof ladderManager === 'undefined') return [];
        return Array.from(ladderManager.instruments);
    }

    /**
     * Subscribe to market data for all instruments
     */
    subscribeToMarketData() {
        if (!this.wsClient || !this.wsClient.isConnected()) {
            console.log('[Dashboard] WebSocket not connected yet, skipping subscription');
            return;
        }

        const instruments = this.getSubscribedInstruments();
        if (instruments.length === 0) {
            console.log('[Dashboard] No instruments to subscribe to yet');
            return;
        }

        console.log('[Dashboard] Subscribing to level2 for instruments:', instruments);
        instruments.forEach(instrument => {
            this.wsClient.send({
                type: 'subscribe',
                channel: 'level2',
                instrument: instrument
            });
        });
    }

    /**
     * Update connection status indicator
     */
    updateConnectionStatus(status) {
        const indicator = document.getElementById('statusIndicator');
        const text = document.getElementById('statusText');

        if (!indicator || !text) return;

        switch (status) {
            case 'connected':
                indicator.className = 'status-indicator connected';
                text.textContent = 'Connected';
                break;
            case 'disconnected':
                indicator.className = 'status-indicator disconnected';
                text.textContent = 'Disconnected';
                break;
            case 'connecting':
                indicator.className = 'status-indicator';
                text.textContent = 'Connecting...';
                break;
        }
    }

    /**
     * Start periodic data refresh
     */
    startPeriodicRefresh() {
        // Refresh P&L and positions every 5 seconds
        this.refreshInterval = setInterval(() => {
            this.loadPnL();
            this.loadPositions();
        }, 5000);
    }

    /**
     * Stop periodic refresh
     */
    stopPeriodicRefresh() {
        if (this.refreshInterval) {
            clearInterval(this.refreshInterval);
            this.refreshInterval = null;
        }
    }

    /**
     * Add log entry
     */
    log(message, level = 'info') {
        const logContainer = document.getElementById('logContainer');
        if (!logContainer) return;

        const timestamp = new Date().toLocaleTimeString('en-US', {
            hour12: false,
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit'
        });

        const entry = document.createElement('div');
        entry.className = `log-entry log-${level}`;
        entry.innerHTML = `
            <span class="log-time">[${timestamp}]</span>
            <span class="log-message">${message}</span>
        `;

        logContainer.appendChild(entry);

        // Auto-scroll to bottom
        logContainer.scrollTop = logContainer.scrollHeight;

        // Limit log entries
        while (logContainer.children.length > 100) {
            logContainer.removeChild(logContainer.firstChild);
        }
    }

    /**
     * Clear log
     */
    clearLog() {
        const logContainer = document.getElementById('logContainer');
        if (logContainer) {
            logContainer.innerHTML = '';
            this.log('Log cleared', 'info');
        }
    }

    /**
     * Load open orders
     */
    async loadOrders() {
        try {
            const orders = await api.getOpenOrders();
            this.updateOrdersTable(orders);
        } catch (error) {
            console.error('[Dashboard] Failed to load orders:', error);
        }
    }

    /**
     * Render open orders table
     */
    updateOrdersTable(orders) {
        const tbody = document.getElementById('ordersTableBody');
        if (!tbody) return;

        if (!orders || orders.length === 0) {
            tbody.innerHTML = '<tr class="empty-state"><td colspan="7">No open orders</td></tr>';
            return;
        }

        tbody.innerHTML = orders.map(order => {
            const sideClass = order.side === 'buy' ? 'positive' : 'negative';
            const qtyDisplay = `${api.formatQuantity(order.remaining_qty, 4)} / ${api.formatQuantity(order.qty, 4)}`;
            return `
                <tr>
                    <td>${order.strategy_id}</td>
                    <td>${order.instrument}</td>
                    <td class="${sideClass}">${order.side.toUpperCase()}</td>
                    <td>${api.formatCurrency(order.price)}</td>
                    <td>${qtyDisplay}</td>
                    <td>${order.status}</td>
                    <td>
                        <div class="action-buttons">
                            <button class="btn btn-sm btn-primary"
                                onclick="modalManager.showModifyOrderModal(
                                    '${order.strategy_id}',
                                    '${order.order_id}',
                                    '${order.client_order_id}',
                                    ${order.price},
                                    ${order.remaining_qty}
                                )">Modify</button>
                            <button class="btn btn-sm btn-danger"
                                onclick="window.dashboard.cancelOrder(
                                    '${order.strategy_id}',
                                    '${order.order_id}',
                                    '${order.client_order_id}'
                                )">Cancel</button>
                        </div>
                    </td>
                </tr>
            `;
        }).join('');
    }

    /**
     * Cancel a specific order
     */
    async cancelOrder(strategyId, orderId, clientOrderId) {
        try {
            await api.cancelOrder(strategyId, orderId, clientOrderId);
            this.log(`Cancel submitted for order ${orderId}`, 'info');
            modalManager.showNotification('Cancel request submitted', 'success');
        } catch (error) {
            this.log(`Cancel failed: ${error.message}`, 'error');
            modalManager.showNotification(`Cancel failed: ${error.message}`, 'error');
        }
    }

    /**
     * Cancel all open orders (with confirmation)
     */
    async confirmCancelAllOrders() {
        if (!confirm('Cancel ALL open orders across all strategies?')) return;
        try {
            const result = await api.cancelAllOrders();
            this.log(`Cancel all submitted (${result.strategies_affected} strategies)`, 'warning');
            modalManager.showNotification('Cancel all request submitted', 'success');
        } catch (error) {
            this.log(`Cancel all failed: ${error.message}`, 'error');
            modalManager.showNotification(`Cancel all failed: ${error.message}`, 'error');
        }
    }

    /**
     * Cleanup
     */
    destroy() {
        this.stopPeriodicRefresh();
        if (this.wsClient) {
            this.wsClient.close();
        }
        chartManager.destroy();
    }
}

// Initialize dashboard when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    window.dashboard = new Dashboard();
    window.dashboard.init().catch(error => {
        console.error('[Dashboard] Initialization failed:', error);
    });
});

// Cleanup on page unload
window.addEventListener('beforeunload', () => {
    if (window.dashboard) {
        window.dashboard.destroy();
    }
});
