// Open orders across strategies: modify, cancel, cancel all.
import { html, useCallback, useState } from '../core/html.js';
import { api, formatPrice, formatQuantity } from '../core/api.js';
import { useOnConnect, useWs } from '../core/connection.js';
import { useCoalesced } from '../core/hooks.js';
import { Dialog, notify } from '../core/notify.js';
import * as bus from '../core/bus.js';

const RELOAD_COALESCE_MS = 100;   // a burst of order updates reloads once

function ModifyDialog({ order, onClose }) {
    const [price, setPrice] = useState(String(order.price));
    const [qty, setQty] = useState(String(order.remaining_qty));
    const submit = async () => {
        const newPrice = parseFloat(price);
        const newQty = parseFloat(qty);
        if (!(newPrice > 0)) return notify('Price must be a positive number', 'error');
        if (!(newQty > 0)) return notify('Quantity must be a positive number', 'error');
        try {
            await api.modifyOrder(order.strategy_id, order.order_id, {
                client_order_id: order.client_order_id, new_price: newPrice, new_quantity: newQty,
            });
            notify('Modify request submitted', 'success');
            bus.log(`Modify submitted: order ${order.order_id}`, 'info');
            onClose();
        } catch (error) {
            notify(`Modify failed: ${error.message}`, 'error');
        }
    };
    return html`
        <${Dialog} title=${`Modify ${order.instrument} ${order.side.toUpperCase()}`} onClose=${onClose}
            footer=${html`<button class="btn" onClick=${onClose}>Cancel</button>
                          <button class="btn btn-primary" onClick=${submit}>Submit modify</button>`}>
            <div class="form">
                <div class="muted mono">${order.strategy_id} · ${order.order_id}</div>
                <div class="form-row">
                    <label class="field"><span>New price</span>
                        <input type="number" step="any" value=${price} onInput=${(e) => setPrice(e.target.value)} />
                    </label>
                    <label class="field"><span>New quantity</span>
                        <input type="number" step="any" value=${qty} onInput=${(e) => setQty(e.target.value)} />
                    </label>
                </div>
                <div class="form-warning">Changes go to the live market.</div>
            </div>
        <//>`;
}

export default function Orders() {
    const [orders, setOrders] = useState([]);
    const [modifying, setModifying] = useState(null);

    const load = useCallback(async () => {
        try {
            setOrders((await api.getOpenOrders()) ?? []);
        } catch (error) {
            console.error('[Orders] Failed to load:', error);
        }
    }, []);
    const reload = useCoalesced(load, RELOAD_COALESCE_MS);
    useOnConnect(load);
    useWs('order_update', reload);

    const cancel = async (o) => {
        try {
            await api.cancelOrder(o.strategy_id, o.order_id, o.client_order_id);
            bus.log(`Cancel submitted for order ${o.order_id}`, 'info');
            notify('Cancel request submitted', 'success');
        } catch (error) {
            bus.log(`Cancel failed: ${error.message}`, 'error');
            notify(`Cancel failed: ${error.message}`, 'error');
        }
    };
    const cancelAll = async () => {
        if (!confirm('Cancel ALL open orders across all strategies?')) return;
        try {
            const result = await api.cancelAllOrders();
            bus.log(`Cancel all submitted (${result.strategies_affected} strategies)`, 'warning');
            notify('Cancel all request submitted', 'success');
        } catch (error) {
            bus.log(`Cancel all failed: ${error.message}`, 'error');
            notify(`Cancel all failed: ${error.message}`, 'error');
        }
    };
    const closeModify = useCallback(() => setModifying(null), []);

    return html`
        <div class="tool-window">
            <div class="tool-bar">
                <span class="muted">${orders.length} open</span>
                <span class="spacer"></span>
                <button class="btn" onClick=${load}>Refresh</button>
                <button class="btn btn-danger" onClick=${cancelAll} disabled=${orders.length === 0}>Cancel all</button>
            </div>
            <div class="tool-body">
                <table class="data-table">
                    <thead><tr>
                        <th>Strategy</th><th>Instrument</th><th>Side</th><th class="num">Price</th>
                        <th class="num">Remaining / Qty</th><th>Status</th><th>Actions</th>
                    </tr></thead>
                    <tbody>
                        ${orders.length === 0 && html`<tr class="empty"><td colspan="7">No open orders</td></tr>`}
                        ${orders.map((o) => html`
                            <tr key=${o.client_order_id}>
                                <td>${o.strategy_id}</td>
                                <td>${o.instrument}</td>
                                <td class=${o.side === 'buy' ? 'positive' : 'negative'}>${o.side.toUpperCase()}</td>
                                <td class="num">${formatPrice(o.price)}</td>
                                <td class="num">${formatQuantity(o.remaining_qty, 4)} / ${formatQuantity(o.qty, 4)}</td>
                                <td>${o.status}</td>
                                <td><div class="actions">
                                    <button class="btn" onClick=${() => setModifying(o)}>Modify</button>
                                    <button class="btn btn-danger" onClick=${() => cancel(o)}>Cancel</button>
                                </div></td>
                            </tr>`)}
                    </tbody>
                </table>
            </div>
            ${modifying && html`<${ModifyDialog} order=${modifying} onClose=${closeModify} />`}
        </div>`;
}
