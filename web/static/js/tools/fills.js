// Fills, newest first: the server's last ones as the window connects, then each as it comes.
import { html, useRef, useState } from '../core/html.js';
import { api, formatPrice, formatQuantity, formatTime } from '../core/api.js';
import { useOnConnect, useWs } from '../core/connection.js';

export const FILLS_KEPT = 1000;   // as many as the server keeps

const isFill = (data) => data.event === 'fill' || data.event === 'leg_fill';

// The fills to show, newest first, from the server's `history` (oldest first) and those that
// came over the socket while it was fetched. Each is shown once: by its seq.
export function mergeFills(history, arrived) {
    const seen = new Set(history.map((f) => f.seq));
    const fresh = arrived.filter((f) => !seen.has(f.seq));
    return history.concat(fresh).sort((a, b) => b.seq - a.seq).slice(0, FILLS_KEPT);
}

export default function Fills() {
    const [fills, setFills] = useState([]);
    const lastSeq = useRef(0);
    const loading = useRef(null);   // fills that came while the history was fetched

    useOnConnect(async () => {
        loading.current = [];
        try {
            const history = (await api.getFills()) ?? [];
            const merged = mergeFills(history, loading.current);
            lastSeq.current = merged[0]?.seq ?? 0;
            setFills(merged);
        } catch (error) {
            console.error('[Fills] Failed to load:', error);
        } finally {
            loading.current = null;
        }
    });
    useWs('order_update', (data) => {
        if (!isFill(data)) {
            return;
        }
        if (loading.current) {
            loading.current.push(data);
        } else if (data.seq > lastSeq.current) {
            lastSeq.current = data.seq;
            setFills((list) => [data, ...list].slice(0, FILLS_KEPT));
        }
    });

    return html`
        <div class="tool-window">
            <div class="tool-bar"><span class="muted">${fills.length} fill${fills.length === 1 ? '' : 's'}</span></div>
            <div class="tool-body">
                <table class="data-table">
                    <thead><tr>
                        <th>Time</th><th>Strategy</th><th>Instrument</th><th>Side</th>
                        <th class="num">Qty</th><th class="num">Price</th><th class="num">Fee</th>
                        <th class="num">Remaining</th><th>Order</th><th>Fill</th>
                    </tr></thead>
                    <tbody>
                        ${fills.length === 0 && html`<tr class="empty"><td colspan="10">No fills</td></tr>`}
                        ${fills.map((f) => html`
                            <tr key=${f.seq}>
                                <td class="mono">${formatTime(f.timestamp_ns)}</td>
                                <td>${f.strategy_id || '—'}</td>
                                <td>${f.instrument}${f.event === 'leg_fill' ? html` <span class="muted">(leg)</span>` : ''}</td>
                                <td class=${f.side === 'buy' ? 'positive' : 'negative'}>${(f.side ?? '').toUpperCase()}</td>
                                <td class="num">${formatQuantity(f.fill_qty, 4)}</td>
                                <td class="num">${formatPrice(f.fill_price)}</td>
                                <td class="num">${f.fee ? formatPrice(f.fee) : '—'}</td>
                                <td class="num">${f.remaining_qty != null ? formatQuantity(f.remaining_qty, 4) : '—'}</td>
                                <td class="mono muted">${f.order_id}</td>
                                <td class="mono muted">${f.fill_id || '—'}</td>
                            </tr>`)}
                    </tbody>
                </table>
            </div>
        </div>`;
}
