// A ladder window: pick an instrument from what the server lists, see its book, click to trade.
// Nothing streams until one is picked: the server has MANUAL subscribe it while a ladder shows
// it, and lets it go a grace period after the last one closes or switches away.
import { html, useCallback, useEffect, useRef, useState } from '../core/html.js';
import { api } from '../core/api.js';
import { ws, useOnConnect, useWs } from '../core/connection.js';
import { useTitle } from '../core/hooks.js';
import { instruments, watchInstruments } from '../core/instruments.js';
import { loadScript } from '../core/load.js';
import { notify } from '../core/notify.js';
import { session } from '../core/store.js';
import * as bus from '../core/bus.js';
import { Picker, useInstruments } from '../components/picker.js';

// Ticks for instruments whose broker names none.
const FALLBACK_TICKS = { 'ES': 0.25, 'NQ': 0.25 };
const DEFAULT_TICK = 0.01;
const TIF = { GTC: '0', IOC: '1', FOK: '2', DAY: '4' };

const COLORS = {
    background: '#1e1e1e',
    bidQtyBackground: '#1a2f3a',
    askQtyBackground: '#3a1a1f',
    priceBackground: '#2a2a2a',
    bidBar: '#4caf50',
    askBar: '#f44336',
    text: '#e0e0e0',
    gridLine: '#444444',
    ownOrderBorder: '#ffd700',
    hoverBackground: 'rgba(255, 255, 255, 0.1)',
};

export function tickFor(instrument) {
    return instruments.tickFor(instrument) || FALLBACK_TICKS[instrument] || DEFAULT_TICK;
}

const sideEnum = (side) => (side === 'buy' ? 0 : 1);

// One instrument's SlickLadder in `container`, and the levels this engine's orders rest at, so
// the ladder borders them.
export class LadderView {
    constructor(container, onLevelClick) {
        this.container = container;
        this.onLevelClick = onLevelClick;
        this.ladder = null;
        this.instrument = null;
        this.pending = new Map();   // order_id -> { instrument, price, sideEnum }
    }

    show(instrument, tickSize) {
        this.destroy();
        this.instrument = instrument;
        if (!instrument) {
            return;
        }
        this.ladder = new SlickLadder.PriceLadder({
            container: this.container,
            width: 200,
            height: Math.max(this.container.clientHeight, 100),
            rowHeight: 20,
            tickSize,
            mode: 'PriceLevel',
            removalMode: 'removeRow',
            showVolumeBars: false,
            showOrderCount: false,
            minQuantityThreshold: 0.0001,
            colors: COLORS,
            onLevelClick: (price, side) => this.onLevelClick(instrument, price, side),
        });
        this.restoreOwnOrders();
    }

    destroy() {
        try {
            this.ladder?.destroy();
        } catch (error) {
            console.error('[Ladder] Error destroying the ladder:', error);
        }
        this.ladder = null;
        this.container.replaceChildren();
    }

    resize(height) {
        // The renderer sizes its canvas to its columns as the quantities change; the ladder's
        // configured width is only the first guess, so passing it back would clip the ask column.
        const ladder = this.ladder;
        if (!ladder) {
            return;
        }
        const h = Math.max(height, 100);
        ladder.config.height = h;
        ladder.renderer.resize(ladder.renderer.width, h);
    }

    clear() {
        this.ladder?.clear();
    }

    // A level2 message: a snapshot replaces the book.
    apply(data) {
        if (!this.ladder || data.instrument !== this.instrument) {
            return;   // on its way when the ladder switched from it
        }
        if (data.is_snapshot) {
            this.ladder.clear();
        }
        if (Array.isArray(data.levels)) {
            this.ladder.processBatch(data.levels.map(([price, qty, side]) => ({
                side: side == 0 ? SlickLadder.Side.BID : SlickLadder.Side.ASK,
                price: parseFloat(price),
                quantity: parseFloat(qty),
                numOrders: data.seq_num,
            })));
            // A level removed and added again lost its own-order flag.
            this.restoreOwnOrders();
        }
    }

    setOwn(instrument, price, side, value) {
        if (this.ladder && instrument === this.instrument) {
            this.ladder.setHasOwnOrders(price, side, value);
        }
    }

    // The orders open as the window connects, from /api/orders.
    seed(orders) {
        this.pending.clear();
        for (const o of orders ?? []) {
            if (o.order_id && o.price) {
                this.pending.set(o.order_id, { instrument: o.instrument, price: o.price, sideEnum: sideEnum(o.side) });
            }
        }
        this.restoreOwnOrders();
    }

    hasOrdersAt(instrument, price, side) {
        for (const o of this.pending.values()) {
            if (o.instrument === instrument && o.price === price && o.sideEnum === side) {
                return true;
            }
        }
        return false;
    }

    forget(orderId) {
        const order = this.pending.get(orderId);
        if (order) {
            this.pending.delete(orderId);
            if (!this.hasOrdersAt(order.instrument, order.price, order.sideEnum)) {
                this.setOwn(order.instrument, order.price, order.sideEnum, false);
            }
        }
    }

    // ack: an order rests at its price, a modify at its new one under the same order_id. A fill
    // that leaves nothing, or a cancel, takes it off.
    onOrderUpdate(data) {
        if (data.event === 'ack' && data.order_id && data.instrument && data.price != null) {
            const side = sideEnum(data.side);
            const previous = this.pending.get(data.order_id);
            this.pending.set(data.order_id, { instrument: data.instrument, price: data.price, sideEnum: side });
            if (previous && (previous.instrument !== data.instrument || previous.price !== data.price ||
                             previous.sideEnum !== side) &&
                !this.hasOrdersAt(previous.instrument, previous.price, previous.sideEnum)) {
                this.setOwn(previous.instrument, previous.price, previous.sideEnum, false);
            }
            this.setOwn(data.instrument, data.price, side, true);
        } else if (data.event === 'fill' && data.order_id && data.remaining_qty === 0) {
            this.forget(data.order_id);
        } else if (data.event === 'cancel' && data.order_id) {
            this.forget(data.order_id);
        }
    }

    restoreOwnOrders() {
        for (const o of this.pending.values()) {
            this.setOwn(o.instrument, o.price, o.sideEnum, true);
        }
    }
}

export default function Ladder() {
    const list = useInstruments();
    const [instrument, setInstrument] = useState(() => session.get('ladder.instrument'));
    const [qty, setQty] = useState('');
    const [price, setPrice] = useState('');
    const [tif, setTif] = useState(TIF.IOC);
    const [ready, setReady] = useState(false);   // slick-ladder loaded
    const host = useRef(null);
    const view = useRef(null);
    const streamed = useRef(null);   // what this window's socket streams
    const controls = useRef({});
    controls.current = { qty, price, tif };

    useTitle(instrument ? `Ladder · ${instrument}` : 'Ladder');
    useEffect(() => watchInstruments(), []);

    // Streams `next`'s book and no other; null streams none. `again` asks for the book once more
    // where it streams already: a ladder built anew starts empty, and the server sends the book
    // whole on every subscribe.
    const stream = useCallback((next, again = false) => {
        const connected = ws.isConnected();
        const target = connected ? next ?? null : null;
        if (target === streamed.current) {
            if (again && target) {
                ws.send({ type: 'subscribe', channel: 'level2', instrument: target });
            }
            return;
        }
        if (streamed.current && connected) {
            ws.send({ type: 'unsubscribe', channel: 'level2', instrument: streamed.current });
        }
        streamed.current = target;
        if (target) {
            ws.send({ type: 'subscribe', channel: 'level2', instrument: target });
        }
    }, []);

    const seedOwnOrders = useCallback(async () => {
        try {
            view.current?.seed(await api.getOpenOrders());
        } catch (error) {
            console.error('[Ladder] Failed to load open orders:', error);
        }
    }, []);

    // The click-to-trade order: at the price typed, else the level clicked, else market.
    const onLevelClick = useCallback(async (inst, levelPrice, side) => {
        const { qty: qtyText, price: priceText, tif: tifValue } = controls.current;
        if (!instruments.config) {
            notify('Order not sent: the ladder config has not loaded yet', 'error');
            return;
        }
        const config = instruments.configFor(inst);
        const quantity = parseFloat(qtyText) || config.default_qty;
        const override = parseFloat(priceText);
        const orderPrice = override > 0 ? override : levelPrice ?? undefined;
        const type = orderPrice !== undefined ? 'limit' : 'market';
        // Left at its default, IOC, a limit order rests GTC; a market order left at GTC goes IOC.
        const timeInForce = type === 'limit' && tifValue === TIF.IOC ? parseInt(TIF.GTC)
                          : type === 'market' && tifValue === TIF.GTC ? parseInt(TIF.IOC)
                          : parseInt(tifValue);
        const brokerId = instruments.brokerFor(inst);
        if (brokerId === undefined) {
            notify(`Order not sent: no broker known for ${inst}`, 'error');
            return;
        }
        const orderSide = side === 0 ? 'buy' : 'sell';
        try {
            await api.submitOrder({
                instrument: inst,
                broker_id: brokerId,
                side: orderSide,
                type,
                quantity,
                time_in_force: timeInForce,
                ...(type === 'limit' && { price: orderPrice }),
            });
            if (type === 'limit') {
                view.current?.setOwn(inst, orderPrice, side, true);
            }
            setPrice('');
            setTif(TIF.IOC);
            const msg = `Order submitted: ${orderSide.toUpperCase()} ${quantity} ${inst}${type === 'limit' ? ` @ ${orderPrice}` : ''}`;
            notify(msg, 'success');
            bus.log(msg, 'success');
        } catch (error) {
            notify(`Order failed: ${error.message}`, 'error');
            bus.log(`Order failed: ${inst} ${error.message}`, 'error');
        }
    }, []);

    useEffect(() => {
        let alive = true;
        loadScript('js/slick-ladder.js').then(() => {
            if (alive) {
                view.current = new LadderView(host.current, onLevelClick);
                setReady(true);
            }
        }).catch((error) => notify(error.message, 'error'));
        const observer = new ResizeObserver(() => view.current?.resize(host.current.clientHeight));
        observer.observe(host.current);
        return () => {
            alive = false;
            observer.disconnect();
            view.current?.destroy();
        };
    }, []);

    // A restored pick no longer listed, after a restart, is dropped.
    const shown = list.loaded && instrument && !list.has(instrument) ? null : instrument;

    // The ladder of what is shown, built anew as it changes or its tick comes, and its book asked
    // for again: the new ladder has none.
    const tick = shown ? tickFor(shown) : 0;
    useEffect(() => {
        if (!ready || !list.loaded) {
            return;
        }
        view.current.show(shown, tick);
        stream(shown, true);
        if (shown) {
            seedOwnOrders();
        }
    }, [ready, list.loaded, shown, tick]);

    // The qty starts at the instrument's default once the ladder config has come, and only then:
    // before it, the default is not known. What is typed after stays until another is picked.
    const configured = list.config !== null;
    const qtyFor = useRef(null);   // the instrument the qty was set for
    useEffect(() => {
        if (configured && shown && qtyFor.current !== shown) {
            qtyFor.current = shown;
            setQty(String(instruments.configFor(shown).default_qty ?? 0.01));
        }
    }, [configured, shown]);

    // A new connection streams nothing yet: the book comes again whole.
    useOnConnect(() => {
        streamed.current = null;
        view.current?.clear();
        if (ready && list.loaded) {
            stream(shown);
            seedOwnOrders();
        }
    });
    useWs('level2', (data) => view.current?.apply(data));
    useWs('order_update', (data) => view.current?.onOrderUpdate(data));
    useWs('subscription_error', (data) => notify(`${data.instrument}: ${data.message}`, 'error'));

    const pick = (name) => {
        session.set('ladder.instrument', name);
        setPrice('');
        setInstrument(name);
    };

    // The qty typed becomes the default for the instrument, as the ladder config keeps it.
    const saveQty = (text) => {
        const value = parseFloat(text);
        const config = instruments.config;
        if (!(value > 0) || !config || !shown) {
            return;
        }
        if (config.per_instrument?.[shown]) {
            config.per_instrument[shown].default_qty = value;
        } else {
            config.default_qty = value;
        }
        api.saveLadderConfig(config).catch((error) => console.error('[Ladder] Failed to save qty:', error));
    };

    return html`
        <div class="tool-window">
            <div class="tool-bar">
                <${Picker} value=${shown} onPick=${pick} autoFocus=${true} />
            </div>
            <div class="tool-bar">
                <label>Qty</label>
                <input type="number" step="any" min="0" value=${qty} title="Order quantity for click-to-trade"
                       disabled=${!configured} placeholder=${configured ? '' : 'Loading…'}
                       onInput=${(e) => setQty(e.target.value)} onChange=${(e) => saveQty(e.target.value)} />
                <label>Price</label>
                <input type="number" step="any" min="0" value=${price} placeholder="Ladder"
                       title="Price to trade at, over the level clicked"
                       onInput=${(e) => { setPrice(e.target.value); setTif(e.target.value.trim() ? TIF.GTC : TIF.IOC); }} />
                <select value=${tif} title="Time in force" onChange=${(e) => setTif(e.target.value)}>
                    ${Object.entries(TIF).map(([label, value]) => html`<option value=${value}>${label}</option>`)}
                </select>
            </div>
            ${!shown && html`<div class="tool-note">${list.loaded ? 'Pick an instrument to see its book.' : 'Loading instruments…'}</div>`}
            <div class=${`ladder-host${shown ? '' : ' hidden'}`} ref=${host}></div>
        </div>`;
}
