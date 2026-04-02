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
            this.loadPnL();
        });

        // Clear log button
        document.getElementById('clearLog')?.addEventListener('click', () => {
            this.clearLog();
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
                this.loadPnL(),
                this.loadPositions(),
                this.loadStrategies()
            ]);

        } catch (error) {
            console.error('[Dashboard] Failed to load initial data:', error);
            this.log('Failed to load initial data: ' + error.message, 'error');
        }
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

        // Aggregate P&L from all strategies
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

        // Track P&L change
        if (this.lastPnLUpdate !== null) {
            const change = totalPnL - this.lastPnLUpdate;
            const changeEl = document.getElementById('pnlChange');
            if (changeEl) {
                changeEl.textContent = change >= 0 ? `+${api.formatCurrency(change)}` : api.formatCurrency(change);
                changeEl.className = 'metric-change ' + api.getColorClass(change);
            }
        }
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
            tbody.innerHTML = '<tr class="empty-state"><td colspan="5">No strategies loaded</td></tr>';
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

            return `
                <tr>
                    <td>${strategyId}</td>
                    <td class="${api.getColorClass(pnl.realized_pnl)}">${api.formatCurrency(pnl.realized_pnl)}</td>
                    <td class="${api.getColorClass(pnl.unrealized_pnl)}">${api.formatCurrency(pnl.unrealized_pnl)}</td>
                    <td class="${api.getColorClass(pnl.total_pnl)}">${api.formatCurrency(pnl.total_pnl)}</td>
                    <td>
                        <div class="action-buttons">
                            <button class="btn btn-sm btn-success" onclick="window.dashboard.controlStrategy('${strategyId}', 'start')" title="Start">▶</button>
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
        const countEl = document.getElementById('activeStrategies');
        if (countEl) {
            const count = Array.isArray(strategies) ? strategies.length : Object.keys(strategies).length;
            countEl.textContent = count;
        }
    }

    /**
     * Handle real-time P&L update
     */
    handlePnLUpdate(data) {
        console.log('[Dashboard] P&L update received:', data);
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
        this.log(`Order ${data.order_id}: ${data.status}`, 'info');
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
