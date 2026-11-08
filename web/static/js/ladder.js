/**
 * Price Ladder Manager
 * Lists the instruments strategies hold, as the server sends them over the WebSocket, and shows
 * one ladder, the selected instrument's: only it is built, and only its book streams. A server
 * listing thousands costs a selector option each.
 */
class LadderManager {
    constructor() {
        this.ladder = null;                   // the selected instrument's ladder, when there is one
        this.instruments = [];                // in order, as the selector lists them
        this.instrumentBrokers = new Map();   // instrument -> id of the broker streaming it
        this.selectedInstrument = null;
        // Called with the instrument whose book to stream, or null for none, as the selection changes
        this.onSelect = null;
        // Maps order_id -> { instrument, price, sideEnum } for clearing hasOwnOrders on fill/cancel
        this.pendingOrders = new Map();
    }

    /**
     * Loads the ladder config the ladder is built with. Before the server lists instruments.
     */
    async loadConfig() {
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
    }

    /**
     * The server's whole list, instrument -> broker id, as it sends it on every connect. A
     * restarted server may have dropped some, or all: those that stay keep the ladder and selection.
     */
    setInstruments(instrumentBrokers) {
        this.instrumentBrokers = new Map(Object.entries(instrumentBrokers ?? {}));
        const instruments = Array.from(this.instrumentBrokers.keys()).sort();
        if (instruments.length === 0) {
            this.showEmptyState('No instruments yet. The ladder lists what strategies subscribe to, and the "instruments" MANUAL\'s config lists.');
        } else if (this.instruments.length === 0) {
            this.build(instruments);
        } else {
            this.reconcileInstruments(instruments);
        }
    }

    /**
     * What the server listed since, instrument -> broker id. Each new one goes into the selector
     * in order; the rest stays as it is.
     */
    addInstruments(instrumentBrokers) {
        const added = Object.keys(instrumentBrokers ?? {}).filter(inst => !this.instrumentBrokers.has(inst)).sort();
        if (added.length === 0) return;
        added.forEach(inst => this.instrumentBrokers.set(inst, instrumentBrokers[inst]));
        if (this.instruments.length === 0) {
            this.build(added);
            return;
        }

        // Both in order: each new one goes before the first listed one past it.
        const select = document.getElementById('instrumentSelect');
        if (!select) return;
        const listed = Array.from(select.options);
        const merged = [];
        let next = 0;
        added.forEach(inst => {
            while (next < this.instruments.length && this.instruments[next] < inst) {
                merged.push(this.instruments[next++]);
            }
            select.insertBefore(new Option(inst, inst), listed[next] ?? null);
            merged.push(inst);
        });
        this.instruments = merged.concat(this.instruments.slice(next));
        console.log(`[LadderManager] ${added.length} instrument(s) added`);
    }

    /**
     * Builds the selector, and the ladder of the first of `instruments`.
     */
    build(instruments) {
        this.instruments = instruments;
        this.selectedInstrument = instruments[0];
        this.createLadderUI();
        this.showLadder(this.selectedInstrument);
        console.log(`[LadderManager] Built the ladder, listing ${instruments.length} instrument(s)`);
    }

    /**
     * Matches the selector to the server's list, in its order: drops what it no longer lists and
     * adds what is new. Linear, as MANUAL's list can take in a venue.
     */
    reconcileInstruments(instruments) {
        const select = document.getElementById('instrumentSelect');
        if (!select) return;

        // A static list: walking the live select.options can cost a lookup per index.
        const allOptions = () => Array.from(select.querySelectorAll('option'));
        const wanted = new Set(instruments);
        const options = new Map(allOptions().map(option => [option.value, option]));
        this.instruments.filter(inst => !wanted.has(inst)).forEach(inst => {
            options.get(inst)?.remove();
            options.delete(inst);
        });

        // Into the selector at once, not an option at a time.
        const added = instruments.filter(inst => !options.has(inst));
        const fresh = document.createDocumentFragment();
        added.forEach(inst => options.set(inst, fresh.appendChild(new Option(inst, inst))));
        select.appendChild(fresh);

        // New ones went to the end: put the selector back in the server's order.
        if (allOptions().some((option, i) => option.value !== instruments[i])) {
            const ordered = document.createDocumentFragment();
            instruments.forEach(inst => ordered.appendChild(options.get(inst)));
            select.appendChild(ordered);
        }
        this.instruments = instruments;
        if (wanted.has(this.selectedInstrument)) {
            select.value = this.selectedInstrument;   // moving options can move the selection
        } else {
            this.switchInstrument(instruments[0]);
        }
        console.log(`[LadderManager] Instruments matched to the server: ${added.length} added`);
    }

    destroyLadder() {
        try {
            this.ladder?.destroy();
        } catch (error) {
            console.error('[LadderManager] Error destroying the ladder:', error);
        }
        this.ladder = null;
    }

    /**
     * The ladder of `instrument`: none unless it is the one shown.
     */
    ladderOf(instrument) {
        return instrument === this.selectedInstrument ? this.ladder : null;
    }

    /**
     * Builds the ladder of `instrument` in place of the one shown, and has its book streamed.
     */
    showLadder(instrument) {
        this.destroyLadder();
        document.getElementById('ladderCanvas')?.replaceChildren();
        this.createLadder(instrument);
        this.onSelect?.(instrument);
    }

    /**
     * Replaces the loading placeholder with why there is no ladder. The server only
     * streams books for instruments a strategy subscribed to.
     */
    showEmptyState(message) {
        // Drops the ladders with the UI they draw in: the next init builds both again.
        this.clear();
        const container = document.getElementById('ladderContainer');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'ladder-loading';
        div.textContent = message;
        container.replaceChildren(div);
    }

    /**
     * The broker an order for `instrument` goes to: the one its own ladder config names, else
     * the broker streaming it. Undefined when neither is known. A default_broker_id at the top
     * of the config is not read: it sent every instrument's orders to one broker.
     */
    brokerFor(instrument) {
        return this.ladderConfig?.per_instrument?.[instrument]?.default_broker_id ?? this.instrumentBrokers.get(instrument);
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
            <!-- <button class="ladder-settings-btn" id="ladderSettingsBtn" title="Ladder Settings">⚙️</button> -->
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
                <option value="4">DAY</option>
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

        // The one div the ladder draws into (no canvas - library creates it)
        const ladderContainer = document.createElement('div');
        ladderContainer.id = 'ladderCanvas';
        ladderContainer.className = 'ladder-canvas-container';
        container.appendChild(ladderContainer);
    }

    /**
     * Create a price ladder for an instrument
     */
    createLadder(instrument) {
        const ladderContainer = document.getElementById('ladderCanvas');
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

            this.ladder = ladder;
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

        this.selectedInstrument = instrument;
        this.showLadder(instrument);

        // Keep the selector in step when code switches, as after the selected instrument is removed
        const select = document.getElementById('instrumentSelect');
        if (select) {
            select.value = instrument;
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
        // None for an instrument no longer shown: its updates were on the way when it was switched from.
        const ladder = this.ladderOf(instrument);
        if (!ladder) return;

        // console.log(`[LadderManager] Updating orderbook with level update for ${instrument}:`, {
        //     levels: data.levels?.length || 0,
        //     is_snapshot: data.is_snapshot
        // });

        try {
            // A snapshot is the whole book: what the ladder held before is gone.
            if (data.is_snapshot) {
                ladder.clear();
            }
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
     * Clear the ladder's order book without destroying it
     */
    clearBooks() {
        try {
            this.ladder?.clear();
        } catch (error) {
            console.error('[LadderManager] Error clearing the ladder:', error);
        }
        console.log('[LadderManager] Cleared the ladder order book');
    }

    /**
     * Drop the ladder and the list
     */
    clear() {
        this.destroyLadder();
        this.instruments = [];
        this.selectedInstrument = null;
        this.onSelect?.(null);
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
        const brokerId = this.brokerFor(instrument);
        if (brokerId === undefined) {
            const msg = `Order not sent: no broker known for ${instrument}`;
            if (window.modalManager) window.modalManager.showNotification(msg, 'error');
            console.error('[LadderManager]', msg);
            return;
        }
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
        const ladder = this.ladderOf(instrument);
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
            // A modify is acked at its NEW price under the SAME order_id, so this
            // is a move as often as it is a new order. Take the order off its old
            // level before recording the new one, or the border stays behind on a
            // price we no longer rest at -- for as long as that level survives in
            // the book, which is forever if anyone else is quoting it.
            const previous = this.pendingOrders.get(data.order_id);
            this.pendingOrders.set(data.order_id, {
                instrument: data.instrument,
                price: data.price,
                sideEnum
            });
            if (previous && (previous.instrument !== data.instrument ||
                             previous.price !== data.price ||
                             previous.sideEnum !== sideEnum) &&
                !this._hasOrdersAtLevel(previous.instrument, previous.price, previous.sideEnum)) {
                this.setOwnOrder(previous.instrument, previous.price, previous.sideEnum, false);
            }
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
