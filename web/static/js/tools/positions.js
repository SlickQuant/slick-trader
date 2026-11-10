// Positions across strategies.
import { html, useCallback, useState } from '../core/html.js';
import { api, colorClass, formatCurrency, formatQuantity } from '../core/api.js';
import { useOnConnect, useWs } from '../core/connection.js';
import { useCoalesced, useInterval } from '../core/hooks.js';

const POLL_MS = 5000;
const AFTER_FILL_MS = 250;   // a fill moves a position: reload once a burst of them is in

export default function Positions() {
    const [rows, setRows] = useState([]);

    const load = useCallback(async () => {
        try {
            const byStrategy = (await api.getAllPositions()) ?? {};
            setRows(Object.entries(byStrategy).flatMap(([strategy, positions]) =>
                positions.map((p) => ({ strategy, ...p }))));
        } catch (error) {
            console.error('[Positions] Failed to load:', error);
        }
    }, []);
    const reload = useCoalesced(load, AFTER_FILL_MS);
    useOnConnect(load);
    useInterval(load, POLL_MS);
    useWs('order_update', (data) => (data.event === 'fill' || data.event === 'leg_fill') && reload());

    return html`
        <div class="tool-window">
            <div class="tool-bar">
                <span class="muted">${rows.length} position${rows.length === 1 ? '' : 's'}</span>
                <span class="spacer"></span>
                <button class="btn" onClick=${load}>Refresh</button>
            </div>
            <div class="tool-body">
                <table class="data-table">
                    <thead><tr>
                        <th>Strategy</th><th>Instrument</th><th class="num">Quantity</th>
                        <th class="num">Avg price</th><th class="num">Realized P&L</th>
                    </tr></thead>
                    <tbody>
                        ${rows.length === 0 && html`<tr class="empty"><td colspan="5">No positions</td></tr>`}
                        ${rows.map((p) => html`
                            <tr key=${`${p.strategy}/${p.instrument}`}>
                                <td>${p.strategy}</td>
                                <td>${p.instrument}</td>
                                <td class=${`num ${colorClass(p.quantity)}`}>${formatQuantity(p.quantity, 4)}</td>
                                <td class="num">${formatCurrency(p.avg_price)}</td>
                                <td class=${`num ${colorClass(p.realized_pnl)}`}>${formatCurrency(p.realized_pnl)}</td>
                            </tr>`)}
                    </tbody>
                </table>
            </div>
        </div>`;
}
