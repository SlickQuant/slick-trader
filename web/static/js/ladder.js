/**
 * Price Ladder Manager
 * Manages multiple price ladders for different instruments
 */
class LadderManager {
    constructor() {
        this.ladders = new Map();
        this.instruments = [];
        this.selectedInstrument = null;
    }

    /**
     * Initialize ladders for loaded strategies
     */
    async init(strategies) {
        console.log('[LadderManager] Initializing with strategies:', strategies);

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

        // Start simulated data for testing (remove when real data is available)
        this.startSimulatedData();
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

        // Create instrument selector
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
        `;
        container.appendChild(selectorDiv);

        // Add event listener for instrument selection
        const select = document.getElementById('instrumentSelect');
        if (select) {
            select.addEventListener('change', (e) => {
                this.switchInstrument(e.target.value);
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
                showVolumeBars: true,
                showOrderCount: true,
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
                }
            });

            this.ladders.set(instrument, ladder);
            console.log(`[LadderManager] Created ladder for ${instrument}`);
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

        console.log(`[LadderManager] Switched to instrument: ${instrument}`);
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
                side: side === 'bid' ? SlickLadder.Side.BID : SlickLadder.Side.ASK,
                price: parseFloat(price),
                quantity: parseFloat(quantity),
                numOrders: numOrders
            });
        } catch (error) {
            console.error(`[LadderManager] Error updating level:`, error);
        }
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
}

// Create global ladder manager instance
const ladderManager = new LadderManager();
