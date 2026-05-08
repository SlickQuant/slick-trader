/**
 * Price Ladder Manager
 * Manages multiple price ladders for different instruments
 */
class LadderManager {
    constructor() {
        this.ladders = new Map();
        this.instruments = [];
        this.selectedInstrument = null;
        // Maps order_id -> { instrument, price, sideEnum } for clearing hasOwnOrders on fill/cancel
        this.pendingOrders = new Map();
    }

    /**
     * Initialize ladders for loaded strategies
     */
    async init(strategies) {
        console.log('[LadderManager] Initializing with strategies:', strategies);

        // Load ladder config from server
        try {
            this.ladderConfig = await api.getLadderConfig();
            console.log('[LadderManager] Loaded ladder config:', this.ladderConfig);
        } catch (error) {
            console.error('[LadderManager] Failed to load ladder config:', error);
            // Use defaults if server config unavailable
            this.ladderConfig = {
                default_qty: 0.01,
                default_strategy_id: '',
                per_instrument: {}
            };
        }

        // Extract unique instruments from strategies
        this.instruments = this.extractInstruments(strategies);

        if (this.instruments.length === 0) {
            console.warn('[LadderManager] No instruments found in strategies');
            return;
        }

        // Select first instrument by default
        this.selectedInstrument = this.instruments[0];

        // Create ladder containers in the UI
        this.createLadderUI();

        // Initialize ladders
        this.instruments.forEach(instrument => {
            this.createLadder(instrument);
        });

        console.log('[LadderManager] Initialized ladders for instruments:', this.instruments);

        // Real market data now provided via WebSocket - simulated data removed
    }

    /**
     * Start simulated orderbook data for testing
     * TODO: Remove this when real market data is integrated
     */
    startSimulatedData() {
        const basePrice = 100000; // Base price for BTC-USD

        const updateSimulatedOrderbook = () => {
            this.instruments.forEach(instrument => {
                const ladder = this.ladders.get(instrument);
                if (!ladder) return;

                const midPrice = instrument.includes('BTC') ? basePrice + (Math.random() - 0.5) * 100 :
                                 instrument.includes('ETH') ? 3500 + (Math.random() - 0.5) * 50 :
                                 100 + (Math.random() - 0.5) * 1;

                // Generate bids (prices below mid)
                const bidUpdates = [];
                for (let i = 0; i < 20; i++) {
                    const offset = (i + 1) * (instrument.includes('BTC') ? 0.01 : 0.01);
                    bidUpdates.push({
                        side: SlickLadder.Side.BID,
                        price: midPrice - offset,
                        quantity: Math.random() * 10,
                        numOrders: Math.floor(Math.random() * 5) + 1
                    });
                }

                // Generate asks (prices above mid)
                const askUpdates = [];
                for (let i = 0; i < 20; i++) {
                    const offset = (i + 1) * (instrument.includes('BTC') ? 0.01 : 0.01);
                    askUpdates.push({
                        side: SlickLadder.Side.ASK,
                        price: midPrice + offset,
                        quantity: Math.random() * 10,
                        numOrders: Math.floor(Math.random() * 5) + 1
                    });
                }

                // Update ladder
                ladder.processBatch([...bidUpdates, ...askUpdates]);
            });
        };

        // Update every 100ms
        this.simulationInterval = setInterval(updateSimulatedOrderbook, 100);
        console.log('[LadderManager] Started simulated data updates');
    }

    /**
     * Stop simulated data
     */
    stopSimulatedData() {
        if (this.simulationInterval) {
            clearInterval(this.simulationInterval);
            this.simulationInterval = null;
            console.log('[LadderManager] Stopped simulated data updates');
        }
    }

    /**
     * Extract instruments from strategy configurations
     */
    extractInstruments(strategies) {
        const instrumentSet = new Set();

        if (Array.isArray(strategies)) {
            strategies.forEach(strategy => {
                // Check if strategy has instruments array (from API)
                if (strategy.instruments && Array.isArray(strategy.instruments)) {
                    strategy.instruments.forEach(inst => {
                        instrumentSet.add(inst);
                    });
                }
                // Also check if strategy has risk_limits with instruments (from config file)
                else if (strategy.risk_limits && strategy.risk_limits.instruments) {
                    Object.keys(strategy.risk_limits.instruments).forEach(inst => {
                        instrumentSet.add(inst);
                    });
                }
                // Also check parameters for instruments
                if (strategy.parameters && strategy.parameters.instruments) {
                    if (Array.isArray(strategy.parameters.instruments)) {
                        strategy.parameters.instruments.forEach(inst => instrumentSet.add(inst));
                    }
                }
            });
        }

        return Array.from(instrumentSet);
    }

    /**
     * Create ladder UI containers
     */
    createLadderUI() {
        const container = document.getElementById('ladderContainer');
        if (!container) {
            console.error('[LadderManager] Ladder container not found');
            return;
        }

        // Clear existing content
        container.innerHTML = '';

        // Row 1: instrument selector + settings button
        const selectorDiv = document.createElement('div');
        selectorDiv.className = 'ladder-selector';
        selectorDiv.innerHTML = `
            <label for="instrumentSelect">Instrument:</label>
            <select id="instrumentSelect" class="instrument-select">
                ${this.instruments.map(inst => `
                    <option value="${inst}" ${inst === this.selectedInstrument ? 'selected' : ''}>
                        ${inst}
                    </option>
                `).join('')}
            </select>
            <button class="ladder-settings-btn" id="ladderSettingsBtn" title="Ladder Settings">⚙️</button>
        `;
        container.appendChild(selectorDiv);

        // Row 2: qty + price inputs
        const orderControlsDiv = document.createElement('div');
        orderControlsDiv.className = 'ladder-order-controls';
        const initialQty = this.getConfigForInstrument(this.selectedInstrument).default_qty ?? 0.01;
        orderControlsDiv.innerHTML = `
            <label for="qtyInput">Qty:</label>
            <input type="number" id="qtyInput" class="qty-input" step="0.00000001" min="0.00000001"
                   value="${initialQty}" placeholder="0.01" title="Order quantity for click-to-trade">
            <label for="priceInput">Price:</label>
            <input type="number" id="priceInput" class="price-input" step="any" min="0"
                   placeholder="Market / ladder price" title="Override price (leave empty to use clicked ladder price)">
            <select id="tifSelect" class="tif-select" title="Time In Force">
                <option value="1" selected>IOC</option>
                <option value="0">GTC</option>
                <option value="2">FOK</option>
            </select>
        `;
        container.appendChild(orderControlsDiv);

        // Add event listener for instrument selection
        const select = document.getElementById('instrumentSelect');
        if (select) {
            select.addEventListener('change', (e) => {
                this.switchInstrument(e.target.value);
            });
        }

        // Add event listener for qty input
        const qtyInput = document.getElementById('qtyInput');
        if (qtyInput) {
            qtyInput.addEventListener('change', (e) => {
                const newQty = parseFloat(e.target.value);
                if (!newQty || newQty <= 0) return;
                const instrument = this.selectedInstrument;
                if (this.ladderConfig.per_instrument && this.ladderConfig.per_instrument[instrument]) {
                    this.ladderConfig.per_instrument[instrument].default_qty = newQty;
                } else {
                    this.ladderConfig.default_qty = newQty;
                }
                this.saveLadderConfig(this.ladderConfig).catch(err => {
                    console.error('[LadderManager] Failed to save qty:', err);
                });
            });
        }

        // Update TIF default when price input changes
        const priceInputEl = document.getElementById('priceInput');
        const tifSelect = document.getElementById('tifSelect');
        if (priceInputEl && tifSelect) {
            priceInputEl.addEventListener('input', () => {
                const hasPrice = priceInputEl.value.trim() !== '';
                tifSelect.value = hasPrice ? '0' : '1'; // GTC when price set, IOC when empty
            });
        }

        // Add event listener for settings button
        const settingsBtn = document.getElementById('ladderSettingsBtn');
        if (settingsBtn) {
            settingsBtn.addEventListener('click', () => {
                if (window.modalManager) {
                    window.modalManager.showLadderSettingsModal();
                } else {
                    console.error('[LadderManager] Modal manager not found');
                }
            });
        }

        // Create ladder container for each instrument (no canvas - library creates it)
        this.instruments.forEach(instrument => {
            const ladderContainer = document.createElement('div');
            ladderContainer.id = `ladder-${instrument}`;
            ladderContainer.className = 'ladder-canvas-container';
            ladderContainer.style.display = instrument === this.selectedInstrument ? 'block' : 'none';

            container.appendChild(ladderContainer);
        });
    }

    /**
     * Create a price ladder for an instrument
     */
    createLadder(instrument) {
        const ladderContainer = document.getElementById(`ladder-${instrument}`);
        if (!ladderContainer) {
            console.error(`[LadderManager] Container not found for ${instrument}`);
            return;
        }

        try {
            const ladder = new SlickLadder.PriceLadder({
                container: ladderContainer,
                width: 380,
                height: 480,
                rowHeight: 20,
                visibleRows: 24,
                tickSize: this.getTickSize(instrument),
                mode: 'PriceLevel',  // Start with aggregated price level mode
                removalMode: 'removeRow',
                showVolumeBars: false,
                showOrderCount: false,
                minQuantityThreshold: 0.0001,
                colors: {
                    background: '#1e1e1e',
                    bidQtyBackground: '#1a2f3a',
                    askQtyBackground: '#3a1a1f',
                    priceBackground: '#2a2a2a',
                    bidBar: '#4caf50',
                    askBar: '#f44336',
                    text: '#e0e0e0',
                    gridLine: '#444444',
                    ownOrderBorder: '#ffd700',
                    hoverBackground: 'rgba(255, 255, 255, 0.1)'
                },
                onLevelClick: (price, side) => {
                    this.handleLadderClick(instrument, price, side);
                }
            });

            this.ladders.set(instrument, ladder);
            console.log(`[LadderManager] Created JavaScript ladder for ${instrument}`);
        } catch (error) {
            console.error(`[LadderManager] Failed to create ladder for ${instrument}:`, error);
        }
    }

    /**
     * Get appropriate tick size for instrument
     */
    getTickSize(instrument) {
        // Map instruments to their typical tick sizes
        const tickSizes = {
            'BTC-USD': 0.01,
            'ETH-USD': 0.01,
            'BTC-USDT': 0.01,
            'ETH-USDT': 0.01,
            'SPY': 0.01,
            'ES': 0.25,
            'NQ': 0.25,
            'default': 0.01
        };

        return tickSizes[instrument] || tickSizes.default;
    }

    /**
     * Switch to a different instrument
     */
    switchInstrument(instrument) {
        if (this.selectedInstrument === instrument) return;

        // Hide current ladder
        if (this.selectedInstrument) {
            const currentContainer = document.getElementById(`ladder-${this.selectedInstrument}`);
            if (currentContainer) {
                currentContainer.style.display = 'none';
            }
        }

        // Show new ladder
        this.selectedInstrument = instrument;
        const newContainer = document.getElementById(`ladder-${instrument}`);
        if (newContainer) {
            newContainer.style.display = 'block';
        }

        // Update qty input to reflect per-instrument or global default
        const qtyInput = document.getElementById('qtyInput');
        if (qtyInput) {
            qtyInput.value = this.getConfigForInstrument(instrument).default_qty ?? 0.01;
        }

        // Clear price override when switching instruments
        const priceInput = document.getElementById('priceInput');
        if (priceInput) {
            priceInput.value = '';
        }

        console.log(`[LadderManager] Switched to instrument: ${instrument}`);
    }

    /**
     * Update ladder with level2 data
     */
    updateOrderbookWithLevelUpdate(instrument, data) {
        const ladder = this.ladders.get(instrument);
        if (!ladder) {
            console.warn(`[LadderManager] No ladder found for ${instrument}`);
            return;
        }

        // console.log(`[LadderManager] Updating orderbook with level update for ${instrument}:`, {
        //     levels: data.levels?.length || 0,
        //     is_snapshot: data.is_snapshot
        // });

        try {
            if (data.levels && Array.isArray(data.levels)) {
                const levelUpdates = data.levels.map(([price, qty, side]) => ({
                    side: side == 0 ? SlickLadder.Side.BID : SlickLadder.Side.ASK,
                    price: parseFloat(price),
                    quantity: parseFloat(qty),
                    numOrders: data.seq_num     // use seq_num as numOrders for debuging
                }));
                ladder.processBatch(levelUpdates);
                // Re-apply own order flags: if a level was removed (qty→0) and later
                // re-added by a new update, its hasOwnOrders resets to false.
                this._restoreOwnOrders(instrument, ladder);
            }
        } catch (error) {
            console.error(`[LadderManager] Error updating ladder for ${instrument}:`, error);
        }
    }

    /**
     * Update ladder with orderbook data
     */
    updateOrderbook(instrument, data) {
        const ladder = this.ladders.get(instrument);
        if (!ladder) {
            console.warn(`[LadderManager] No ladder found for ${instrument}`);
            return;
        }

        console.log(`[LadderManager] Updating orderbook for ${instrument}:`, {
            bids: data.bids?.length || 0,
            asks: data.asks?.length || 0,
            is_snapshot: data.is_snapshot
        });

        try {
            // Process bid updates
            if (data.bids && Array.isArray(data.bids)) {
                const bidUpdates = data.bids.map(([price, qty]) => ({
                    side: SlickLadder.Side.BID,
                    price: parseFloat(price),
                    quantity: parseFloat(qty),
                    numOrders: 1
                }));
                ladder.processBatch(bidUpdates);
            }

            // Process ask updates
            if (data.asks && Array.isArray(data.asks)) {
                const askUpdates = data.asks.map(([price, qty]) => ({
                    side: SlickLadder.Side.ASK,
                    price: parseFloat(price),
                    quantity: parseFloat(qty),
                    numOrders: 1
                }));
                ladder.processBatch(askUpdates);
            }
        } catch (error) {
            console.error(`[LadderManager] Error updating ladder for ${instrument}:`, error);
        }
    }

    /**
     * Update ladder with individual price level
     */
    updateLevel(instrument, side, price, quantity, numOrders = 1) {
        const ladder = this.ladders.get(instrument);
        if (!ladder) return;

        try {
            ladder.processUpdate({
                side: side === 0 ? SlickLadder.Side.BID : SlickLadder.Side.ASK,
                price: parseFloat(price),
                quantity: parseFloat(quantity),
                numOrders: numOrders
            });
        } catch (error) {
            console.error(`[LadderManager] Error updating level:`, error);
        }
    }

    /**
     * Clear order book data on all ladders without destroying them
     */
    clearBooks() {
        this.ladders.forEach((ladder, instrument) => {
            try {
                ladder.clear();
            } catch (error) {
                console.error(`[LadderManager] Error clearing ladder for ${instrument}:`, error);
            }
        });
        console.log('[LadderManager] Cleared all ladder order books');
    }

    /**
     * Clear all ladders
     */
    clear() {
        this.stopSimulatedData();
        this.ladders.forEach((ladder, instrument) => {
            try {
                ladder.destroy();
            } catch (error) {
                console.error(`[LadderManager] Error destroying ladder for ${instrument}:`, error);
            }
        });
        this.ladders.clear();
        this.instruments = [];
        this.selectedInstrument = null;
    }

    /**
     * Get metrics for a specific ladder
     */
    getMetrics(instrument) {
        const ladder = this.ladders.get(instrument);
        if (!ladder) return null;

        try {
            return ladder.getMetrics();
        } catch (error) {
            console.error(`[LadderManager] Error getting metrics:`, error);
            return null;
        }
    }

    /**
     * Resize a ladder
     */
    resize(instrument, width, height) {
        const ladder = this.ladders.get(instrument);
        if (!ladder) return;

        try {
            ladder.resize(width, height);
        } catch (error) {
            console.error(`[LadderManager] Error resizing ladder:`, error);
        }
    }

    /**
     * Resize all ladders
     */
    resizeAll(width, height) {
        this.ladders.forEach((ladder, instrument) => {
            this.resize(instrument, width, height);
        });
    }

    /**
     * Get ladder config for specific instrument
     * Merges global config with instrument-specific overrides
     */
    getConfigForInstrument(instrument) {
        if (!this.ladderConfig) {
            return { default_qty: 0.01, default_strategy_id: '' };
        }

        // Check for instrument-specific config
        if (this.ladderConfig.per_instrument && this.ladderConfig.per_instrument[instrument]) {
            return {
                ...this.ladderConfig,
                ...this.ladderConfig.per_instrument[instrument]
            };
        }

        return this.ladderConfig;
    }

    /**
     * Save ladder configuration to server
     */
    async saveLadderConfig(newConfig) {
        try {
            await api.saveLadderConfig(newConfig);
            this.ladderConfig = newConfig;
            console.log('[LadderManager] Ladder config saved successfully');
            return true;
        } catch (error) {
            console.error('[LadderManager] Failed to save ladder config:', error);
            throw error;
        }
    }

    /**
     * Handle click-to-trade on ladder
     * @param {string} instrument - Instrument being traded
     * @param {number} price - Clicked price level
     * @param {number} side - SlickLadder.Side enum (0=BID, 1=ASK)
     */
    async handleLadderClick(instrument, price, side) {
        console.log(`[LadderManager] Ladder clicked - Instrument: ${instrument}, Price: ${price}, Side: ${side}`);

        // Get config for this instrument
        const config = this.getConfigForInstrument(instrument);

        // Validate config has strategy ID
        if (!config.default_strategy_id || config.default_strategy_id.trim() === '') {
            if (window.modalManager) {
                window.modalManager.showNotification(
                    'Please configure a strategy ID in ladder settings (⚙️ icon)',
                    'error'
                );
            }
            return;
        }

        // Convert SlickLadder side to order side
        const orderSide = side === 0 ? 'buy' : 'sell';

        // Determine order type and price:
        // - Price input has a value -> limit at that price
        // - Price input empty and ladder price is not null -> limit at ladder price
        // - Price input empty and ladder price is null -> market order
        const priceInputEl = document.getElementById('priceInput');
        const priceOverride = priceInputEl ? parseFloat(priceInputEl.value) : NaN;

        let orderType, orderPrice;
        if (!isNaN(priceOverride) && priceOverride > 0) {
            orderType = 'limit';
            orderPrice = priceOverride;
        } else if (price !== null && price !== undefined) {
            orderType = 'limit';
            orderPrice = price;
        } else {
            orderType = 'market';
            orderPrice = undefined;
        }

        // Build order payload
        const brokerId = config.default_broker_id ?? 1;
        const tifSelectEl = document.getElementById('tifSelect');
        // If TIF is still at the default (IOC/market default), auto-set based on order type.
        // This handles the case where the ladder price is used (priceInput is empty so the
        // input event never fired to switch TIF to GTC).
        if (tifSelectEl) {
            const currentTif = parseInt(tifSelectEl.value);
            const defaultMarketTif = 1; // IOC
            const defaultLimitTif = 0;  // GTC
            if (orderType === 'limit' && currentTif === defaultMarketTif) {
                tifSelectEl.value = String(defaultLimitTif);
            } else if (orderType === 'market' && currentTif === defaultLimitTif) {
                tifSelectEl.value = String(defaultMarketTif);
            }
        }
        const timeInForce = tifSelectEl ? parseInt(tifSelectEl.value) : (orderType === 'limit' ? 0 : 1);
        const payload = {
            strategy_id: config.default_strategy_id,
            instrument: instrument,
            broker_id: brokerId,
            side: orderSide,
            type: orderType,
            quantity: config.default_qty,
            time_in_force: timeInForce,
            ...(orderType === 'limit' && { price: orderPrice })
        };

        try {
            await api.submitOrder(payload);
            if (orderType === 'limit') {
                this.setOwnOrder(instrument, orderPrice, side, true);
            }
            if (priceInputEl) priceInputEl.value = '';
            if (tifSelectEl) tifSelectEl.value = '1'; // Reset to IOC after order
            const priceStr = orderType === 'limit' ? ` @ ${orderPrice}` : '';
            const msg = `Order submitted: ${orderSide.toUpperCase()} ${config.default_qty} ${instrument}${priceStr}`;
            if (window.modalManager) window.modalManager.showNotification(msg, 'success');
            if (window.dashboard) window.dashboard.log(msg, 'success');
            // Refresh positions after a short delay to capture fill
            setTimeout(() => {
                if (window.dashboard) {
                    window.dashboard.loadPositions();
                    window.dashboard.loadPnL();
                }
            }, 1500);
        } catch (error) {
            const msg = `Order failed: ${error.message}`;
            if (window.modalManager) window.modalManager.showNotification(msg, 'error');
            console.error('[LadderManager]', msg);
        }
    }
    /**
     * Set or clear hasOwnOrders on a price level in the ladder for an instrument.
     * @param {string} instrument
     * @param {number} price
     * @param {number} sideEnum - 0 for BID/buy, 1 for ASK/sell
     * @param {boolean} value
     */
    setOwnOrder(instrument, price, sideEnum, value) {
        const ladder = this.ladders.get(instrument);
        if (!ladder) return;
        ladder.setHasOwnOrders(price, sideEnum, value);
    }

    /**
     * Handle order_update WebSocket messages.
     * - ack: store order in pendingOrders map
     * - fill (remaining_qty === 0): clear hasOwnOrders
     * - cancel: clear hasOwnOrders
     * @param {Object} data
     */
    onOrderUpdate(data) {
        if (data.event === 'ack' && data.order_id && data.instrument && data.price != null) {
            const sideEnum = data.side === 'buy' ? 0 : 1;
            this.pendingOrders.set(data.order_id, {
                instrument: data.instrument,
                price: data.price,
                sideEnum
            });
            this.setOwnOrder(data.instrument, data.price, sideEnum, true);
        } else if (data.event === 'fill' && data.order_id) {
            if (data.remaining_qty === 0) {
                const order = this.pendingOrders.get(data.order_id);
                if (order) {
                    this.pendingOrders.delete(data.order_id);
                    // Only clear the flag if no other orders remain at this price level
                    if (!this._hasOrdersAtLevel(order.instrument, order.price, order.sideEnum)) {
                        this.setOwnOrder(order.instrument, order.price, order.sideEnum, false);
                    }
                }
            }
        } else if (data.event === 'cancel' && data.order_id) {
            const order = this.pendingOrders.get(data.order_id);
            if (order) {
                this.pendingOrders.delete(data.order_id);
                // Only clear the flag if no other orders remain at this price level
                if (!this._hasOrdersAtLevel(order.instrument, order.price, order.sideEnum)) {
                    this.setOwnOrder(order.instrument, order.price, order.sideEnum, false);
                }
            }
        }
    }

    /** Returns true if pendingOrders still has any order at the given instrument+price+side. */
    _hasOrdersAtLevel(instrument, price, sideEnum) {
        for (const o of this.pendingOrders.values()) {
            if (o.instrument === instrument && o.price === price && o.sideEnum === sideEnum) {
                return true;
            }
        }
        return false;
    }

    /**
     * Re-apply hasOwnOrders flags for all pending orders on this instrument.
     * Called after processBatch because a level removed at qty=0 and later re-added
     * will have its flag reset to false by the ladder internals.
     */
    _restoreOwnOrders(instrument, ladder) {
        for (const o of this.pendingOrders.values()) {
            if (o.instrument === instrument) {
                ladder.setHasOwnOrders(o.price, o.sideEnum, true);
            }
        }
    }
}

// Create global ladder manager instance
const ladderManager = new LadderManager();
window.ladderManager = ladderManager;
