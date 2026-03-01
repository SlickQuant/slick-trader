/**
 * Modal dialog management
 */
class ModalManager {
    constructor() {
        this.activeModal = null;
    }

    /**
     * Show order submission modal
     */
    showOrderModal(strategy_id = '', instrument = '') {
        const modal = this.createOrderModal(strategy_id, instrument);
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

        // Update price field visibility based on order type
        const orderTypeSelect = form.querySelector('#orderType');
        const priceField = form.querySelector('.price-field');

        orderTypeSelect.addEventListener('change', (e) => {
            priceField.style.display = e.target.value === 'limit' ? 'block' : 'none';
        });
    }

    /**
     * Create order modal HTML
     */
    createOrderModal(strategy_id, instrument) {
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

                        <div class="form-row">
                            <div class="form-group">
                                <label for="side">Side</label>
                                <select id="side" required>
                                    <option value="buy">Buy</option>
                                    <option value="sell">Sell</option>
                                </select>
                            </div>

                            <div class="form-group">
                                <label for="orderType">Type</label>
                                <select id="orderType" required>
                                    <option value="market">Market</option>
                                    <option value="limit">Limit</option>
                                </select>
                            </div>
                        </div>

                        <div class="form-group">
                            <label for="quantity">Quantity</label>
                            <input type="number" id="quantity" step="0.00000001" required placeholder="0.01">
                        </div>

                        <div class="form-group price-field" style="display: none;">
                            <label for="price">Price</label>
                            <input type="number" id="price" step="0.01" placeholder="50000.00">
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
        const side = form.querySelector('#side').value;
        const orderType = form.querySelector('#orderType').value;
        const quantity = parseFloat(form.querySelector('#quantity').value);
        const price = orderType === 'limit' ? parseFloat(form.querySelector('#price').value) : 0;

        // Validation
        if (!strategyId || !instrument || !quantity) {
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
                side: side,
                type: orderType,
                quantity: quantity,
                price: price
            });

            this.showNotification(`Order submitted: ${side} ${quantity} ${instrument}`, 'success');
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
