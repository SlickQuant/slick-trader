/**
 * Modal dialog management
 */
class ModalManager {
    constructor() {
        this.activeModal = null;
    }

    /**
     * Show order submission modal
     * @param {string} strategy_id - Strategy ID
     * @param {string} instrument - Instrument symbol
     * @param {object} prefillData - Optional prefill data {side, price, quantity, orderType}
     */
    showOrderModal(strategy_id = '', instrument = '', prefillData = null) {
        const modal = this.createOrderModal(strategy_id, instrument, prefillData);
        document.body.appendChild(modal);
        this.activeModal = modal;

        // Setup event listeners
        const closeBtn = modal.querySelector('.modal-close');
        const cancelBtn = modal.querySelector('.btn-cancel');
        const submitBtn = modal.querySelector('.btn-submit');
        const form = modal.querySelector('.order-form');

        closeBtn.addEventListener('click', () => this.closeModal());
        cancelBtn.addEventListener('click', () => this.closeModal());

        submitBtn.addEventListener('click', () => this.submitOrder(form));

        // Close on backdrop click
        modal.addEventListener('click', (e) => {
            if (e.target === modal) {
                this.closeModal();
            }
        });

        // Close on escape
        document.addEventListener('keydown', this.handleEscape);

        // Update price field visibility and TIF options based on order type
        const orderTypeSelect = form.querySelector('#orderType');
        const priceField = form.querySelector('.price-field');
        const tifSelect = form.querySelector('#timeInForce');

        // Find the form-row containing the price field
        const quantityPriceRow = priceField.closest('.form-row');

        const updateFieldsForOrderType = (orderType) => {
            if (orderType === 'limit') {
                priceField.classList.remove('hidden');
                quantityPriceRow.style.gridTemplateColumns = 'repeat(2, 1fr)';
                // Limit orders: show all TIF options
                tifSelect.innerHTML = `
                    <option value="0">GTC (Good Till Cancel)</option>
                    <option value="1">IOC (Immediate Or Cancel)</option>
                    <option value="2">FOK (Fill Or Kill)</option>
                    <option value="3">GTD (Good Till Date)</option>
                `;
            } else {
                priceField.classList.add('hidden');
                quantityPriceRow.style.gridTemplateColumns = '1fr';
                // Market orders: only IOC and FOK
                tifSelect.innerHTML = `
                    <option value="1">IOC (Immediate Or Cancel)</option>
                    <option value="2">FOK (Fill Or Kill)</option>
                `;
            }
        };

        // Initialize fields based on current order type
        updateFieldsForOrderType(orderTypeSelect.value);

        orderTypeSelect.addEventListener('change', (e) => {
            updateFieldsForOrderType(e.target.value);
        });
    }

    /**
     * Create order modal HTML
     */
    createOrderModal(strategy_id, instrument, prefillData = null) {
        // Apply prefill data if provided
        const side = prefillData?.side || 'buy';
        const orderType = prefillData?.orderType || (prefillData?.price ? 'limit' : 'market');
        const quantity = prefillData?.quantity || '';
        const price = prefillData?.price || '';
        const priceHidden = orderType === 'market' ? 'hidden' : '';

        const modal = document.createElement('div');
        modal.className = 'modal';
        modal.innerHTML = `
            <div class="modal-content">
                <div class="modal-header">
                    <h3>Submit Order</h3>
                    <button class="modal-close">&times;</button>
                </div>
                <div class="modal-body">
                    <form class="order-form">
                        <div class="form-group">
                            <label for="strategyId">Strategy</label>
                            <input type="text" id="strategyId" value="${strategy_id}" required placeholder="Strategy ID">
                        </div>

                        <div class="form-group">
                            <label for="instrument">Instrument</label>
                            <input type="text" id="instrument" value="${instrument}" required placeholder="BTC-USD">
                        </div>

                        <div class="form-group">
                            <label for="brokerId">Broker</label>
                            <select id="brokerId" required>
                                <option value="1">Coinbase</option>
                                <option value="2">Alpaca</option>
                                <option value="3">Simulated</option>
                            </select>
                        </div>

                        <div class="form-row">
                            <div class="form-group">
                                <label for="side">Side</label>
                                <select id="side" required>
                                    <option value="buy" ${side === 'buy' ? 'selected' : ''}>Buy</option>
                                    <option value="sell" ${side === 'sell' ? 'selected' : ''}>Sell</option>
                                </select>
                            </div>

                            <div class="form-group">
                                <label for="orderType">Type</label>
                                <select id="orderType" required>
                                    <option value="market" ${orderType === 'market' ? 'selected' : ''}>Market</option>
                                    <option value="limit" ${orderType === 'limit' ? 'selected' : ''}>Limit</option>
                                </select>
                            </div>
                        </div>

                        <div class="form-group">
                            <label for="timeInForce">Time In Force</label>
                            <select id="timeInForce" required>
                                <option value="1">IOC (Immediate Or Cancel)</option>
                                <option value="2">FOK (Fill Or Kill)</option>
                                ${orderType === 'limit' ? '<option value="0">GTC (Good Till Cancel)</option><option value="3">GTD (Good Till Date)</option>' : ''}
                            </select>
                        </div>

                        <div class="form-row">
                            <div class="form-group">
                                <label for="quantity">Quantity</label>
                                <input type="number" id="quantity" step="0.00000001" required placeholder="0.01" value="${quantity}">
                            </div>

                            <div class="form-group price-field ${priceHidden}">
                                <label for="price">Price</label>
                                <input type="number" id="price" step="0.01" placeholder="50000.00" value="${price}">
                            </div>
                        </div>

                        <div class="form-warning">
                            ⚠️ Orders will be submitted to the live market. Use with caution!
                        </div>
                    </form>
                </div>
                <div class="modal-footer">
                    <button class="btn btn-cancel">Cancel</button>
                    <button class="btn btn-primary btn-submit">Submit Order</button>
                </div>
            </div>
        `;
        return modal;
    }

    /**
     * Submit order to API
     */
    async submitOrder(form) {
        const strategyId = form.querySelector('#strategyId').value;
        const instrument = form.querySelector('#instrument').value;
        const brokerId = parseInt(form.querySelector('#brokerId').value);
        const side = form.querySelector('#side').value;
        const orderType = form.querySelector('#orderType').value;
        const quantity = parseFloat(form.querySelector('#quantity').value);
        const price = orderType === 'limit' ? parseFloat(form.querySelector('#price').value) : NaN;
        const timeInForce = parseInt(form.querySelector('#timeInForce').value);

        // Validation
        if (!strategyId || !instrument || !brokerId || !quantity || timeInForce === undefined) {
            this.showNotification('Please fill in all required fields', 'error');
            return;
        }

        if (quantity <= 0) {
            this.showNotification('Quantity must be greater than zero', 'error');
            return;
        }

        if (orderType === 'limit' && (!price || price <= 0)) {
            this.showNotification('Limit orders require a valid price', 'error');
            return;
        }

        try {
            const result = await api.submitOrder({
                strategy_id: strategyId,
                instrument: instrument,
                broker_id: brokerId,
                side: side,
                type: orderType,
                quantity: quantity,
                time_in_force: timeInForce,
                ...(orderType === 'limit' && { price: price })
            });

            this.showNotification(`Order submitted: ${side} ${quantity} ${instrument}`, 'success');
            if (orderType === 'limit' && typeof ladderManager !== 'undefined') {
                const sideEnum = side === 'buy' ? 0 : 1;
                ladderManager.setOwnOrder(instrument, price, sideEnum, true);
            }
            this.closeModal();

            // Log to dashboard
            if (window.dashboard) {
                window.dashboard.log(`Order submitted: ${side.toUpperCase()} ${quantity} ${instrument}`, 'success');
            }

        } catch (error) {
            this.showNotification(`Order failed: ${error.message}`, 'error');
        }
    }

    /**
     * Close active modal
     */
    closeModal() {
        if (this.activeModal) {
            this.activeModal.remove();
            this.activeModal = null;
            document.removeEventListener('keydown', this.handleEscape);
        }
    }

    /**
     * Handle escape key
     */
    handleEscape = (e) => {
        if (e.key === 'Escape') {
            this.closeModal();
        }
    }

    /**
     * Show ladder settings modal
     */
    showLadderSettingsModal() {
        const currentConfig = window.ladderManager?.ladderConfig || { default_qty: 0.01, default_strategy_id: '' };

        const modal = document.createElement('div');
        modal.className = 'modal';
        modal.innerHTML = `
            <div class="modal-content settings-modal">
                <div class="modal-header">
                    <h3>Ladder Trading Settings</h3>
                    <button class="modal-close">&times;</button>
                </div>
                <div class="modal-body">
                    <form class="settings-form">
                        <div class="form-group">
                            <label for="defaultStrategyId">Default Strategy ID</label>
                            <input type="text" id="defaultStrategyId"
                                   value="${currentConfig.default_strategy_id}" required placeholder="SimpleLoggerStrategy">
                            <small>Strategy to use for click-to-trade orders</small>
                        </div>
                    </form>
                </div>
                <div class="modal-footer">
                    <button class="btn btn-cancel">Cancel</button>
                    <button class="btn btn-primary btn-save">Save Settings</button>
                </div>
            </div>
        `;

        document.body.appendChild(modal);
        this.activeModal = modal;

        // Setup event listeners
        const closeBtn = modal.querySelector('.modal-close');
        const cancelBtn = modal.querySelector('.btn-cancel');
        const saveBtn = modal.querySelector('.btn-save');
        const form = modal.querySelector('.settings-form');

        closeBtn.addEventListener('click', () => this.closeModal());
        cancelBtn.addEventListener('click', () => this.closeModal());
        saveBtn.addEventListener('click', () => this.saveLadderSettings(form));

        // Close on backdrop click
        modal.addEventListener('click', (e) => {
            if (e.target === modal) {
                this.closeModal();
            }
        });

        // Close on escape
        document.addEventListener('keydown', this.handleEscape);
    }

    /**
     * Save ladder settings to server
     */
    async saveLadderSettings(form) {
        const defaultStrategyId = form.querySelector('#defaultStrategyId').value.trim();

        if (!defaultStrategyId) {
            this.showNotification('Default strategy ID is required', 'error');
            return;
        }

        try {
            const newConfig = {
                default_qty: window.ladderManager?.ladderConfig?.default_qty ?? 0.01,
                default_strategy_id: defaultStrategyId,
                per_instrument: window.ladderManager?.ladderConfig?.per_instrument || {}
            };

            await window.ladderManager.saveLadderConfig(newConfig);
            this.showNotification('Ladder settings saved successfully', 'success');
            this.closeModal();

        } catch (error) {
            this.showNotification(`Failed to save settings: ${error.message}`, 'error');
        }
    }

    /**
     * Show notification toast
     */
    showNotification(message, type = 'info') {
        const notification = document.createElement('div');
        notification.className = `notification notification-${type}`;
        notification.textContent = message;

        document.body.appendChild(notification);

        // Trigger animation
        setTimeout(() => notification.classList.add('show'), 10);

        // Auto-remove after 3 seconds
        setTimeout(() => {
            notification.classList.remove('show');
            setTimeout(() => notification.remove(), 300);
        }, 3000);
    }
}

// Create global modal manager instance
const modalManager = new ModalManager();
window.modalManager = modalManager;
