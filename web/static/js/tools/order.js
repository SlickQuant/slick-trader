// The order ticket: one order for any listed instrument, booked to MANUAL as every order from the
// dashboard is. Opened prefilled from the URL, or, already open, from the bus.
import { html, useEffect, useState } from '../core/html.js';
import { api } from '../core/api.js';
import { useOnConnect } from '../core/connection.js';
import { useTitle } from '../core/hooks.js';
import { instruments, watchInstruments } from '../core/instruments.js';
import { notify } from '../core/notify.js';
import * as bus from '../core/bus.js';
import { Picker, useInstruments } from '../components/picker.js';

const LIMIT_TIF = [['0', 'GTC'], ['1', 'IOC'], ['2', 'FOK'], ['3', 'GTD']];
const MARKET_TIF = [['1', 'IOC'], ['2', 'FOK']];

const BLANK = { instrument: '', side: 'buy', type: 'market', price: '', qty: '', tif: '1', broker: '' };

// The fields prefill params name, of instrument, side, price and qty: the rest stay.
function fromParams(get) {
    const fields = {};
    for (const key of ['instrument', 'qty']) {
        const value = get(key);
        if (value != null) {
            fields[key] = String(value);
        }
    }
    const side = get('side');
    if (side != null) {
        fields.side = side === 'sell' ? 'sell' : 'buy';
    }
    const price = get('price');
    if (price != null) {
        fields.price = String(price);
        fields.type = fields.price ? 'limit' : 'market';
    }
    return fields;
}

export default function Order({ params }) {
    useTitle('Ticket');
    const list = useInstruments();
    const [form, setForm] = useState(() => ({ ...BLANK, ...fromParams((k) => params.get(k)) }));
    const [brokers, setBrokers] = useState([]);
    const [sending, setSending] = useState(false);
    const set = (patch) => setForm((f) => ({ ...f, ...patch }));

    useEffect(() => watchInstruments(), []);
    useEffect(() => bus.on('tool-params', (m) => {
        if (m.tool === 'order') {
            setForm((f) => ({ ...f, ...fromParams((k) => m.params[k]), broker: '' }));
            window.focus();
        }
    }), []);

    useOnConnect(async () => {
        try {
            setBrokers((await api.getBrokers()).brokers ?? []);
        } catch (error) {
            console.error('[Order] Failed to load brokers:', error);
        }
    });

    // The broker the instrument trades at, when it takes orders, else the first that does.
    const usable = brokers.filter((b) => b.order.enabled);
    const known = String(list.brokerFor(form.instrument) ?? '');
    const broker = form.broker || (usable.find((b) => String(b.id) === known) ?? usable[0])?.id || '';
    const tifs = form.type === 'limit' ? LIMIT_TIF : MARKET_TIF;
    const tif = tifs.some(([v]) => v === form.tif) ? form.tif : tifs[0][0];

    const submit = async (e) => {
        e.preventDefault();
        const quantity = parseFloat(form.qty);
        const price = parseFloat(form.price);
        if (!broker) return notify('No broker here takes orders', 'error');
        if (!form.instrument) return notify('Pick an instrument', 'error');
        if (!(quantity > 0)) return notify('Quantity must be greater than zero', 'error');
        if (form.type === 'limit' && !(price > 0)) return notify('Limit orders require a valid price', 'error');
        setSending(true);
        try {
            await api.submitOrder({
                instrument: form.instrument,
                broker_id: parseInt(broker),
                side: form.side,
                type: form.type,
                quantity,
                time_in_force: parseInt(tif),
                ...(form.type === 'limit' && { price }),
            });
            const msg = `Order submitted: ${form.side.toUpperCase()} ${quantity} ${form.instrument}` +
                (form.type === 'limit' ? ` @ ${price}` : '');
            notify(msg, 'success');
            bus.log(msg, 'success');
        } catch (error) {
            notify(`Order failed: ${error.message}`, 'error');
            bus.log(`Order failed: ${form.instrument} ${error.message}`, 'error');
        } finally {
            setSending(false);
        }
    };

    const brokerLabel = (b) => (!b.order.enabled ? `${b.name} (no order entry)` : !b.order.connected ? `${b.name} (disconnected)` : b.name);
    return html`
        <form class="form" onSubmit=${submit}>
            <div class="field"><span>Instrument</span>
                <${Picker} value=${form.instrument || null} onPick=${(name) => set({ instrument: name, broker: '' })} />
            </div>
            <label class="field"><span>Broker</span>
                <select value=${broker} onChange=${(e) => set({ broker: e.target.value })}>
                    ${brokers.length === 0 && html`<option value="">No brokers registered</option>`}
                    ${brokers.map((b) => html`<option value=${b.id} disabled=${!b.order.enabled}>${brokerLabel(b)}</option>`)}
                </select>
            </label>
            <div class="side-toggle">
                <button type="button" class=${`btn buy${form.side === 'buy' ? ' on' : ''}`} onClick=${() => set({ side: 'buy' })}>Buy</button>
                <button type="button" class=${`btn sell${form.side === 'sell' ? ' on' : ''}`} onClick=${() => set({ side: 'sell' })}>Sell</button>
            </div>
            <div class="form-row">
                <label class="field"><span>Type</span>
                    <select value=${form.type} onChange=${(e) => set({ type: e.target.value })}>
                        <option value="market">Market</option>
                        <option value="limit">Limit</option>
                    </select>
                </label>
                <label class="field"><span>Time in force</span>
                    <select value=${tif} onChange=${(e) => set({ tif: e.target.value })}>
                        ${tifs.map(([value, label]) => html`<option value=${value}>${label}</option>`)}
                    </select>
                </label>
            </div>
            <div class="form-row">
                <label class="field"><span>Quantity</span>
                    <input type="number" step="any" min="0" value=${form.qty} placeholder="0.01"
                           onInput=${(e) => set({ qty: e.target.value })} />
                </label>
                <label class=${`field${form.type === 'limit' ? '' : ' hidden'}`}><span>Price</span>
                    <input type="number" step="any" min="0" value=${form.price}
                           onInput=${(e) => set({ price: e.target.value })} />
                </label>
            </div>
            <div class="form-warning">Orders go to the live market, booked to MANUAL.</div>
            <button type="submit" class="btn btn-primary" disabled=${sending}>${sending ? 'Sending…' : 'Submit order'}</button>
        </form>`;
}
