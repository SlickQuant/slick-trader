// A ladder window: pick an instrument from what the server lists, see its book, click to trade.
// Nothing streams until one is picked: the server has MANUAL subscribe it while a ladder shows
// it, and lets it go a grace period after the last one closes or switches away.
import { html, useCallback, useEffect, useRef, useState } from '../core/html.js';
import { api, formatPrice } from '../core/api.js';
import { ws, useOnConnect, useWs } from '../core/connection.js';
import { useTitle } from '../core/hooks.js';
import { GRID_BROKERS, defaultTifFor, instruments, watchInstruments } from '../core/instruments.js';
import { loadScript } from '../core/load.js';
import { Dialog, notify } from '../core/notify.js';
import { local, session } from '../core/store.js';
import { setWindowParams } from '../core/windows.js';
import * as bus from '../core/bus.js';
import { Picker, useInstruments } from '../components/picker.js';

// Ticks for instruments whose broker names none.
const FALLBACK_TICKS = { 'ES': 0.25, 'NQ': 0.25 };
const DEFAULT_TICK = 0.01;
const TIF = { GTC: '0', IOC: '1', FOK: '2', DAY: '4' };
const TIF_NAMES = Object.fromEntries(Object.entries(TIF).map(([name, value]) => [value, name]));
// The exchanges in GRID_BROKERS have fixed grids of price levels: their ladder keeps an empty row
// for a price with nothing on it, so rows stay put as levels come and go.

// The gear's settings, each ladder's own. Auto-center moves rows under the mouse as the midpoint
// moves, so it starts off. A click asks before it sends an order unless confirmOrders is off.
const SETTINGS_KEY = 'ladder.settings';
const DEFAULT_SETTINGS = { confirmOrders: true, autoCenter: false, emptyRows: 'venue', rowHeight: 20 };
const EMPTY_ROWS = { venue: 'Show on CME and ICE', show: 'Show', hide: 'Hide' };
const ROW_HEIGHTS = [16, 18, 20, 24, 28];
const sameSettings = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// This window's settings, kept across a reload, else those a workspace opened it with. Keys it no
// longer knows are dropped, such as volumeBars (slick-ladder 0.6.0 draws bars only in MBO mode).
function initialSettings(params) {
    let saved = session.get(SETTINGS_KEY);
    if (!saved) {
        try {
            saved = JSON.parse(params.get('settings'));
        } catch {
            // the defaults
        }
    }
    const known = Object.entries(saved ?? {}).filter(([key]) => key in DEFAULT_SETTINGS);
    return { ...DEFAULT_SETTINGS, ...Object.fromEntries(known) };
}

const COLORS = {
    background: '#1e1e1e',
    bidQtyBackground: '#1a2f3a',
    askQtyBackground: '#3a1a1f',
    priceBackground: '#2a2a2a',
    text: '#e0e0e0',
    gridLine: '#444444',
    hoverBackground: 'rgba(255, 255, 255, 0.1)',
};   // merged over the library's, which color the Work, LTQ and header cells

export function tickFor(instrument) {
    return instruments.tickFor(instrument) || FALLBACK_TICKS[instrument] || DEFAULT_TICK;
}

// The ladder rounds a price to its tick in floating point: 4.991 comes back as 4.9910000000000005,
// and sums of quantities drift the same way. Both have at most 8 decimals, as price_t and qty_t keep them.
const clean = (value) => Number(value.toFixed(8));

// In 'showEmpty' mode the library knows no level for a row with nothing on it, so a click there
// would carry no price and go out as a market order. The price the row is drawn with stands in.
function keepEmptyRowPrices(renderer) {
    // With auto-center off the library centres on the midpoint, or on 100 when a side is missing,
    // and keeps that centre: a book that starts one-sided would sit around 100. Centre on the side
    // there is.
    const resolve = renderer.resolveReferencePrice.bind(renderer);
    renderer.resolveReferencePrice = (snapshot) => {
        if (renderer.centerPrice === 0 && !(snapshot.midPrice > 0) && renderer.tickSize > 0) {
            const best = snapshot.bids.length > 0 ? Math.max(...snapshot.bids.map((l) => l.price))
                       : snapshot.asks.length > 0 ? Math.min(...snapshot.asks.map((l) => l.price)) : 0;
            if (best > 0) {
                renderer.centerPrice = Math.round(best / renderer.tickSize) * renderer.tickSize;
            }
        }
        return resolve(snapshot);
    };

    const levelInfo = renderer.rowToLevelInfo.bind(renderer);
    renderer.rowToLevelInfo = (row) => {
        const info = levelInfo(row);
        if (info || renderer.removalMode !== 'showEmpty') {
            return info;
        }
        const price = renderer.rowPrice(row);
        return price === null ? null : { price, quantity: 0, side: SlickLadder.Side.BID };
    };
}

const sideEnum = (side) => (side === 'buy' ? 0 : 1);   // SlickLadder.Side: BID 0, ASK 1
const SIDE_NAMES = ['BUY', 'SELL'];
const MANUAL = 'MANUAL';

// One of MANUAL's orders resting at a price, as the Work column shows it: from /api/orders or an
// ack, with `remaining` left of it. Null for another strategy's, a market order, one not acked yet
// (no order_id to cancel it by) or one with nothing left.
function workingOrder(o, remaining) {
    if (o.strategy_id !== MANUAL || !o.order_id || !(o.price > 0) || !(remaining > 0)) {
        return null;
    }
    return {
        orderId: o.order_id, clientOrderId: o.client_order_id, strategyId: o.strategy_id,
        instrument: o.instrument, price: o.price, side: sideEnum(o.side), remaining,
    };
}

// One instrument's SlickLadder in `container`: its book, MANUAL's working orders in the Work
// column and the last trade in LTQ. `handlers`: onLevelClick(instrument, price, side), and for the
// Work column onWorkCancel(orders) on a left click, onWorkList(price) on a right one, and
// onOrdersChanged() as MANUAL's orders change.
export class LadderView {
    constructor(container, handlers) {
        this.container = container;
        this.handlers = handlers;
        this.ladder = null;
        this.instrument = null;
        this.tick = 0;
        this.orders = new Map();   // order_id -> workingOrder(), every instrument's
        this.lastTrade = null;     // the shown instrument's, as processTrade() takes it
    }

    show(instrument, tickSize, settings) {
        this.destroy();
        this.instrument = instrument;
        this.tick = tickSize;
        this.lastTrade = null;   // the server sends it again after the book
        if (!instrument) {
            return;
        }
        const emptyRows = settings.emptyRows === 'venue' ? GRID_BROKERS.has(instruments.brokerFor(instrument))
                                                         : settings.emptyRows === 'show';
        const { Left, Middle } = SlickLadder.ClickButton;
        this.ladder = new SlickLadder.PriceLadder({
            container: this.container,
            height: Math.max(this.container.clientHeight, 100),
            rowHeight: settings.rowHeight,
            tickSize,
            mode: 'PriceLevel',
            removalMode: emptyRows ? 'showEmpty' : 'removeRow',
            autoCenter: settings.autoCenter,
            showOrderCount: false,
            minQuantityThreshold: 0.0001,
            colors: COLORS,
            // Only a left click trades. Returning nothing lets the ladder re-center on a middle click.
            onLevelClick: (price, side, button) => {
                if (button === Left) {
                    this.handlers.onLevelClick(instrument, price === null ? null : clean(price), side);
                }
            },
            // A left click cancels the orders on the row, both sides, and a right click lists them.
            // A middle click, or one on a row with none, is left to the ladder.
            onWorkClick: (price, button) => {
                const orders = button === Middle ? [] : this.ordersAt(price);
                if (orders.length === 0) {
                    return false;
                }
                if (button === Left) {
                    this.handlers.onWorkCancel(orders);
                } else {
                    this.handlers.onWorkList(clean(price));
                }
                return true;
            },
        });
        keepEmptyRowPrices(this.ladder.renderer);
        this.pushRows(this.shownRows());
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

    // A new connection: the server sends the book and the last trade again.
    clear() {
        this.lastTrade = null;
        if (this.ladder) {
            this.ladder.clear();
            this.pushRows(this.shownRows());
        }
    }

    // A level2 message: a snapshot replaces the book. ladder.clear() takes the Work column and the
    // LTQ with it, so they are put back.
    apply(data) {
        if (!this.ladder || data.instrument !== this.instrument) {
            return;   // on its way when the ladder switched from it
        }
        if (data.is_snapshot) {
            this.ladder.clear();
            this.pushRows(this.shownRows());
            if (this.lastTrade) {
                this.ladder.processTrade(this.lastTrade);
            }
        }
        if (Array.isArray(data.levels)) {
            this.ladder.processBatch(data.levels.map(([price, qty, side]) => ({
                side: side == 0 ? SlickLadder.Side.BID : SlickLadder.Side.ASK,
                price: parseFloat(price),
                quantity: parseFloat(qty),
                numOrders: data.seq_num,
            })));
        }
    }

    // A trade message: the LTQ column shows its qty at its price, colored by who crossed the spread.
    onTrade(data) {
        if (!this.ladder || data.instrument !== this.instrument) {
            return;
        }
        const aggressor = data.side === 0 ? SlickLadder.Side.BID : data.side === 1 ? SlickLadder.Side.ASK : null;
        this.lastTrade = { price: data.price, quantity: data.qty, aggressor };
        this.ladder.processTrade(this.lastTrade);
    }

    // The orders open as the window connects, from /api/orders.
    seed(orders) {
        const before = this.shownRows();
        this.orders.clear();
        for (const o of orders ?? []) {
            const order = workingOrder(o, o.remaining_qty);
            if (order) {
                this.orders.set(order.orderId, order);
            }
        }
        this.pushRows(new Map([...before, ...this.shownRows()]));
        this.handlers.onOrdersChanged?.();
    }

    // ack: an order rests at its price, a modify at its new one under the same order_id. A fill
    // leaves less of it, or nothing, and a cancel or reject takes it off.
    onOrderUpdate(data) {
        const id = data.order_id;
        if (!id) {
            return;
        }
        if (data.event === 'ack') {
            this.set(id, workingOrder(data, data.remaining_qty ?? data.qty));
        } else if (data.event === 'fill') {
            const order = this.orders.get(id);
            if (order) {
                this.set(id, data.remaining_qty > 0 ? { ...order, remaining: data.remaining_qty } : null);
            }
        } else if (data.event === 'cancel' || data.event === 'reject') {
            this.set(id, null);
        }
    }

    // MANUAL's orders on the shown instrument whose Work row is the one at `price`, bids first.
    ordersAt(price) {
        if (!this.ladder || price === null) {
            return [];
        }
        const half = this.tick / 2;
        return [...this.orders.values()]
            .filter((o) => o.instrument === this.instrument && Math.abs(this.rowOf(o) - price) < half)
            .sort((a, b) => a.side - b.side || b.price - a.price);
    }

    // Puts `next` in place of order `id`, or takes it off with null, and redraws the Work rows it
    // left and came to.
    set(id, next) {
        const previous = this.orders.get(id);
        if (!previous && !next) {
            return;
        }
        if (next) {
            this.orders.set(id, next);
        } else {
            this.orders.delete(id);
        }
        for (const o of [previous, next]) {
            if (o && this.ladder && o.instrument === this.instrument) {
                this.pushRow(o.side, this.rowOf(o));
            }
        }
        this.handlers.onOrdersChanged?.();
    }

    // The price of the row an order shows on.
    rowOf(order) {
        return clean(this.ladder.displayPrice(order.price, order.side));
    }

    // The shown instrument's Work rows, as [side, price] by a key of both.
    shownRows() {
        const rows = new Map();
        if (this.ladder) {
            for (const o of this.orders.values()) {
                if (o.instrument === this.instrument) {
                    const price = this.rowOf(o);
                    rows.set(`${o.side}:${price}`, [o.side, price]);
                }
            }
        }
        return rows;
    }

    pushRows(rows) {
        for (const [side, price] of rows.values()) {
            this.pushRow(side, price);
        }
    }

    // One Work cell: the total left of the orders on its row and side, and how many. 0 clears it.
    pushRow(side, price) {
        let qty = 0;
        let count = 0;
        for (const o of this.orders.values()) {
            if (o.instrument === this.instrument && o.side === side && this.rowOf(o) === price) {
                qty += o.remaining;
                ++count;
            }
        }
        this.ladder.setWorkingOrders(price, side, clean(qty), count);
    }
}

function SettingsDialog({ settings, onSave, onClose }) {
    const [draft, setDraft] = useState(settings);
    const set = (key, value) => setDraft((d) => ({ ...d, [key]: value }));
    return html`
        <${Dialog} title="Ladder settings" onClose=${onClose}
            footer=${html`<button class="btn" onClick=${onClose}>Cancel</button>
                          <button class="btn btn-primary" onClick=${() => onSave(draft)}>Save</button>`}>
            <div class="form">
                <label class="check">
                    <input type="checkbox" checked=${draft.confirmOrders} onChange=${(e) => set('confirmOrders', e.target.checked)} />
                    Confirm orders
                </label>
                <div class="form-hint">${draft.confirmOrders
                    ? 'A click on a level asks before it sends the order.'
                    : 'A click on a level sends the order at once.'}</div>
                <label class="check">
                    <input type="checkbox" checked=${draft.autoCenter} onChange=${(e) => set('autoCenter', e.target.checked)} />
                    Auto center
                </label>
                <div class="form-hint">${draft.autoCenter
                    ? 'Keeps the bid/ask midpoint on the middle row, so rows move as it moves. The wheel does not scroll.'
                    : 'The wheel scrolls. Space or a middle-click on the ladder centers it.'}</div>
                <label class="field"><span>Empty price rows</span>
                    <select value=${draft.emptyRows} onChange=${(e) => set('emptyRows', e.target.value)}>
                        ${Object.entries(EMPTY_ROWS).map(([value, label]) => html`<option value=${value}>${label}</option>`)}
                    </select>
                </label>
                <label class="field"><span>Row height</span>
                    <select value=${draft.rowHeight} onChange=${(e) => set('rowHeight', parseInt(e.target.value))}>
                        ${ROW_HEIGHTS.map((h) => html`<option value=${h}>${h}px</option>`)}
                    </select>
                </label>
                <div class="form-hint">For this ladder only. A workspace saved with it keeps them.</div>
            </div>
        <//>`;
}

// The second click of a double-click lands on the dialog the first one opened: a confirm this soon
// after it opens is ignored.
const ARM_MS = 500;

// The order a click made, sent on Buy/Sell or Enter. Enter on a focused button does that button's
// job. Neither button gets focus when the dialog opens: Space would press it, and Space centers
// the ladder.
function ConfirmDialog({ order, onConfirm, onClose }) {
    const opened = useRef(performance.now());
    const confirm = () => {
        if (performance.now() - opened.current >= ARM_MS) {
            onConfirm();
        }
    };
    useEffect(() => {
        const onKey = (e) => {
            if (e.key === 'Enter' && e.target.tagName !== 'BUTTON') {
                e.preventDefault();
                confirm();
            }
        };
        document.addEventListener('keydown', onKey);
        return () => document.removeEventListener('keydown', onKey);
    }, [onConfirm]);
    const limit = order.type === 'limit';
    return html`
        <${Dialog} title="Confirm order" onClose=${onClose}
            footer=${html`<button class="btn" onClick=${onClose}>Cancel</button>
                          <button class=${`btn ${order.side} on`} onClick=${confirm}>${order.side === 'buy' ? 'Buy' : 'Sell'}</button>`}>
            <div class="order-confirm">
                <div class="order-confirm-main">
                    <span class=${order.side}>${order.side.toUpperCase()}</span> ${order.quantity} ${order.instrument}${limit ? ` @ ${order.price}` : ''}
                </div>
                <div class="order-confirm-detail">${limit ? 'Limit' : 'Market'}, ${TIF_NAMES[order.time_in_force] ?? order.time_in_force}</div>
            </div>
        <//>`;
}

// What a cancel of `orders` names in the log: the order, or how many and the row they were on.
function describe(orders) {
    const o = orders[0];
    return orders.length === 1 ? `${SIDE_NAMES[o.side]} ${o.remaining} ${o.instrument} @ ${o.price}`
                               : `${orders.length} orders on ${o.instrument} @ ${o.price}`;
}

// The end of an order ID, enough to tell a row's orders apart: Coinbase's are 36-char UUIDs, too
// wide for a ladder's window. The whole one is the cell's tooltip.
const shortId = (id) => (id.length > 12 ? `…${id.slice(-8)}` : id);

// A right click on a Work cell: MANUAL's orders on that row, each to cancel, or all at once. The
// rows follow fills and cancels as they come, and the dialog closes once none is left.
function WorkOrdersDialog({ instrument, price, orders, onCancel, onClose }) {
    return html`
        <${Dialog} title=${`Working orders: ${instrument} @ ${price}`} onClose=${onClose}
            footer=${html`<button class="btn" onClick=${onClose}>Close</button>
                          <button class="btn btn-danger" onClick=${() => onCancel(orders)}>Cancel all</button>`}>
            <table class="data-table">
                <thead><tr>
                    <th>Side</th><th class="num">Price</th><th class="num">Remaining</th><th>Order</th><th></th>
                </tr></thead>
                <tbody>
                    ${orders.map((o) => html`
                        <tr key=${o.orderId}>
                            <td class=${o.side === 0 ? 'positive' : 'negative'}>${SIDE_NAMES[o.side]}</td>
                            <td class="num">${formatPrice(o.price)}</td>
                            <td class="num">${o.remaining}</td>
                            <td class="mono muted" title=${o.orderId}>${shortId(o.orderId)}</td>
                            <td><button class="btn btn-danger" onClick=${() => onCancel([o])}>Cancel</button></td>
                        </tr>`)}
                </tbody>
            </table>
        <//>`;
}

export default function Ladder({ params }) {
    const list = useInstruments();
    // The one picked here, kept across a reload, else the one a workspace opened it on.
    const [instrument, setInstrument] = useState(() => session.get('ladder.instrument') ?? params.get('instrument'));
    const [qty, setQty] = useState('');
    const [price, setPrice] = useState('');
    const [tifChoice, setTif] = useState('');   // what was picked; '' is the broker's default
    const [showControls, setShowControls] = useState(() => local.get('ladder.controls', false));
    const [settings, setSettings] = useState(() => initialSettings(params));
    const [editing, setEditing] = useState(false);   // the settings dialog is open
    const [confirming, setConfirming] = useState(null);   // the order waiting for a confirm
    const [listing, setListing] = useState(null);   // the Work row price whose orders the dialog lists
    const [, setOrdersSeen] = useState(0);   // bumped as MANUAL's orders change, while it lists them
    const [ready, setReady] = useState(false);   // slick-ladder loaded
    const host = useRef(null);
    const view = useRef(null);
    const streamed = useRef(null);   // what this window's socket streams
    const controls = useRef({});
    const listingRef = useRef(null);
    listingRef.current = listing;

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

    // Sends `order`, then clears the price and TIF: they last one order.
    const send = useCallback(async (order) => {
        try {
            await api.submitOrder(order);
            setPrice('');
            setTif('');
            const msg = `Order submitted: ${order.side.toUpperCase()} ${order.quantity} ${order.instrument}${order.type === 'limit' ? ` @ ${order.price}` : ''}`;
            notify(msg, 'success');
            bus.log(msg, 'success');
        } catch (error) {
            notify(`Order failed: ${error.message}`, 'error');
            bus.log(`Order failed: ${order.instrument} ${error.message}`, 'error');
        }
    }, []);

    // The click-to-trade order: at the price typed, else the level clicked, else market. When the
    // settings ask for a confirm, it waits for one, unchanged from what the click built.
    const onLevelClick = useCallback((inst, levelPrice, side) => {
        const { qty: qtyText, price: priceText, tif: tifValue, confirm } = controls.current;
        if (!instruments.config) {
            notify('Order not sent: the ladder config has not loaded yet', 'error');
            return;
        }
        const config = instruments.configFor(inst);
        const quantity = parseFloat(qtyText) || instruments.defaultQtyFor(inst);
        const override = parseFloat(priceText);
        const orderPrice = override > 0 ? override : levelPrice ?? undefined;
        const type = orderPrice !== undefined ? 'limit' : 'market';
        // A market order cannot rest: left at GTC or DAY, it goes IOC.
        const timeInForce = type === 'market' && (tifValue === TIF.GTC || tifValue === TIF.DAY)
                          ? parseInt(TIF.IOC)
                          : parseInt(tifValue);
        const brokerId = instruments.brokerFor(inst);
        if (brokerId === undefined) {
            notify(`Order not sent: no broker known for ${inst}`, 'error');
            return;
        }
        const order = {
            instrument: inst,
            broker_id: brokerId,
            side: side === 0 ? 'buy' : 'sell',
            type,
            quantity,
            time_in_force: timeInForce,
            ...(type === 'limit' && { price: orderPrice }),
        };
        if (confirm) {
            setConfirming(order);
        } else {
            send(order);
        }
    }, []);

    // Cancels `orders`, all at once: a click on a Work cell, or the dialog's. Each stays on the
    // ladder until its broker says it's cancelled.
    const cancelOrders = useCallback(async (orders) => {
        const results = await Promise.allSettled(
            orders.map((o) => api.cancelOrder(o.strategyId, o.orderId, o.clientOrderId)));
        const failed = orders.filter((_, i) => results[i].status === 'rejected');
        const sent = orders.filter((_, i) => results[i].status === 'fulfilled');
        if (sent.length > 0) {
            const msg = `Cancel submitted: ${describe(sent)}`;
            notify(msg, 'success');
            bus.log(msg, 'info');
        }
        if (failed.length > 0) {
            const reason = results.find((r) => r.status === 'rejected').reason.message;
            const msg = `Cancel failed: ${describe(failed)}: ${reason}`;
            notify(msg, 'error');
            bus.log(msg, 'error');
        }
    }, []);

    useEffect(() => {
        let alive = true;
        loadScript('js/slick-ladder.js').then(() => {
            if (alive) {
                view.current = new LadderView(host.current, {
                    onLevelClick,
                    onWorkCancel: cancelOrders,
                    onWorkList: setListing,
                    onOrdersChanged: () => listingRef.current !== null && setOrdersSeen((n) => n + 1),
                });
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
    const tif = tifChoice || defaultTifFor(shown ? instruments.brokerFor(shown) : undefined);
    // confirmOrders doesn't change how the ladder looks, so toggling it doesn't rebuild the ladder.
    const { confirmOrders, ...look } = settings;
    const lookKey = JSON.stringify(look);
    controls.current = { qty, price, tif, confirm: confirmOrders };
    useTitle(shown ?? 'Ladder');
    useEffect(() => setWindowParams({
        ...(shown && { instrument: shown }),
        ...(!sameSettings(settings, DEFAULT_SETTINGS) && { settings: JSON.stringify(settings) }),
    }), [shown, settings]);

    // The ladder of what is shown, built anew as it changes, its tick comes or its look changes,
    // and its book asked for again: the new ladder has none.
    const tick = shown ? tickFor(shown) : 0;
    useEffect(() => {
        if (!ready || !list.loaded) {
            return;
        }
        view.current.show(shown, tick, look);
        stream(shown, true);
        if (shown) {
            seedOwnOrders();
        }
    }, [ready, list.loaded, shown, tick, lookKey]);

    // The qty starts at the instrument's default once the ladder config and the instruments have
    // come, and only then: before them, the default is not known. What is typed after stays until
    // another is picked.
    const configured = list.config !== null;
    // A qty set so, not typed, follows the default as it changes: a minimum quantity can come late.
    const qtyFor = useRef(null);    // the instrument the qty was set for
    const qtyAuto = useRef(false);  // the qty is the default, not typed
    const defaultQty = configured && list.loaded && shown ? String(instruments.defaultQtyFor(shown)) : null;
    useEffect(() => {
        if (defaultQty !== null && (qtyFor.current !== shown || qtyAuto.current)) {
            qtyFor.current = shown;
            qtyAuto.current = true;
            setQty(defaultQty);
        }
    }, [defaultQty, shown]);

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
    useWs('trade', (data) => view.current?.onTrade(data));
    useWs('order_update', (data) => view.current?.onOrderUpdate(data));
    useWs('subscription_error', (data) => notify(`${data.instrument}: ${data.message}`, 'error'));

    // What the dialog lists: none once the last of them is gone, or the ladder shows another.
    const listed = listing !== null ? view.current?.ordersAt(listing) ?? [] : [];
    useEffect(() => {
        if (listing !== null && listed.length === 0) {
            setListing(null);
        }
    }, [listing, listed.length]);

    const pick = (name) => {
        session.set('ladder.instrument', name);
        setPrice('');
        setInstrument(name);
    };

    // The qty typed becomes the default for that instrument, as the ladder config keeps it.
    const saveQty = (text) => {
        const value = parseFloat(text);
        const config = instruments.config;
        if (!(value > 0) || !config || !shown) {
            return;
        }
        config.per_instrument = config.per_instrument ?? {};
        config.per_instrument[shown] = { ...config.per_instrument[shown], default_qty: value };
        api.saveLadderConfig(config).catch((error) => console.error('[Ladder] Failed to save qty:', error));
    };

    const toggleControls = () => {
        const next = !showControls;
        local.set('ladder.controls', next);
        if (!next) {
            setPrice('');   // an override out of sight must not apply to the next click
            setTif('');
        }
        setShowControls(next);
    };

    // Unchanged settings keep the ladder: a new one would flash empty until its book comes.
    const saveSettings = (next) => {
        setEditing(false);
        if (!sameSettings(next, settings)) {
            session.set(SETTINGS_KEY, next);
            setSettings(next);
        }
    };

    return html`
        <div class="tool-window">
            <div class="tool-bar">
                <${Picker} value=${shown} onPick=${pick} autoFocus=${true} />
                <button class="btn" title="Ladder settings" onClick=${() => setEditing(true)}>⚙</button>
                <button class="btn" title=${showControls ? 'Hide qty, price and TIF' : 'Show qty, price and TIF'}
                        onClick=${toggleControls}>${showControls ? '▴' : '▾'}</button>
            </div>
            ${showControls && html`<div class="tool-bar">
                <span class="pair">
                    <label>Qty</label>
                    <input type="number" step="any" min="0" value=${qty} title="Order quantity for click-to-trade"
                           disabled=${!configured} placeholder=${configured ? '' : 'Loading…'}
                           onInput=${(e) => { qtyAuto.current = false; setQty(e.target.value); }} onChange=${(e) => saveQty(e.target.value)} />
                </span>
                <span class="pair">
                    <label>Price</label>
                    <input type="number" step="any" min="0" value=${price} placeholder="Ladder"
                           title="Price to trade at, over the level clicked"
                           onInput=${(e) => setPrice(e.target.value)} />
                </span>
                <select value=${tif} title="Time in force" onChange=${(e) => setTif(e.target.value)}>
                    ${Object.entries(TIF).map(([label, value]) => html`<option value=${value}>${label}</option>`)}
                </select>
            </div>`}
            ${!shown && html`<div class="tool-note">${list.loaded ? 'Pick an instrument to see its book.' : 'Loading instruments…'}</div>`}
            <div class=${`ladder-host${shown ? '' : ' hidden'}`} ref=${host}></div>
            ${editing && html`<${SettingsDialog} settings=${settings} onSave=${saveSettings} onClose=${() => setEditing(false)} />`}
            ${confirming && html`<${ConfirmDialog} order=${confirming} onClose=${() => setConfirming(null)}
                                                   onConfirm=${() => { setConfirming(null); send(confirming); }} />`}
            ${listed.length > 0 && html`<${WorkOrdersDialog} instrument=${shown} price=${listing} orders=${listed}
                                                onCancel=${cancelOrders} onClose=${() => setListing(null)} />`}
        </div>`;
}
