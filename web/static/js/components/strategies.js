// Strategy control: each strategy's status, P&L, start and stop, and its parameters.
import { html, useCallback, useState } from '../core/html.js';
import { api, colorClass, formatCurrency } from '../core/api.js';
import { useOnConnect } from '../core/connection.js';
import { useInterval } from '../core/hooks.js';
import { Dialog, notify } from '../core/notify.js';

const STATUS_POLL_MS = 5000;   // a halt or a pause is not pushed

export function Strategies({ pnl, log }) {
    const [strategies, setStrategies] = useState([]);
    const [editing, setEditing] = useState(null);   // { id, text, error }

    const load = useCallback(async () => {
        try {
            const list = await api.getStrategies();
            setStrategies(Array.isArray(list) ? list : Object.values(list ?? {}));
        } catch (error) {
            console.error('[Strategies] Failed to load:', error);
        }
    }, []);
    useOnConnect(load);
    useInterval(load, STATUS_POLL_MS);

    const control = async (id, action) => {
        try {
            await api.controlStrategy(id, action);
            log(`Strategy ${id}: ${action}`, 'success');
            notify(`Strategy ${action} successful`, 'success');
            load();
        } catch (error) {
            log(`Strategy control failed: ${error.message}`, 'error');
            notify(`Failed to ${action} strategy`, 'error');
        }
    };

    const configure = async (id) => {
        try {
            const params = await api.getStrategyConfig(id);
            setEditing({ id, text: JSON.stringify(params, null, 2), error: '' });
        } catch (error) {
            log(`Failed to load config for ${id}`, 'error');
        }
    };

    const save = async () => {
        let params;
        try {
            params = JSON.parse(editing.text);
        } catch (e) {
            setEditing({ ...editing, error: `Invalid JSON: ${e.message}` });
            return;
        }
        try {
            await api.putStrategyConfig(editing.id, params);
            log(`Config updated for ${editing.id}`, 'success');
            setEditing(null);
        } catch (error) {
            setEditing({ ...editing, error: error.message || 'Save failed' });
        }
    };
    const close = useCallback(() => setEditing(null), []);

    return html`
        <section class="panel">
            <div class="panel-header">
                <h3>Strategies</h3>
                <button class="btn" onClick=${load}>Refresh</button>
            </div>
            <div class="table-wrap">
                <table class="data-table">
                    <thead><tr>
                        <th>Strategy</th><th>Status</th>
                        <th class="num">Realized</th><th class="num">Unrealized</th><th class="num">Total</th>
                        <th>Actions</th>
                    </tr></thead>
                    <tbody>
                        ${strategies.length === 0 && html`<tr class="empty"><td colspan="6">No strategies</td></tr>`}
                        ${strategies.map(s => {
                            const id = s.name || s.id;
                            const p = pnl[id] ?? {};
                            const held = s.status === 'halted' || s.status === 'paused';
                            const label = held ? (s.halt_reason || s.status).split(':')[0].trim() : (s.status || 'unknown');
                            return html`
                                <tr key=${id}>
                                    <td>${id}</td>
                                    <td><span class=${`status-badge status-${s.status || 'unknown'}`}
                                              title=${held ? (s.halt_reason || '') : ''}>${label}</span></td>
                                    <td class=${`num ${colorClass(p.realized_pnl)}`}>${formatCurrency(p.realized_pnl)}</td>
                                    <td class=${`num ${colorClass(p.unrealized_pnl)}`}>${formatCurrency(p.unrealized_pnl)}</td>
                                    <td class=${`num ${colorClass(p.total_pnl)}`}>${formatCurrency(p.total_pnl)}</td>
                                    <td><div class="actions">
                                        <button class="btn btn-success" title="Start" disabled=${s.status === 'running'}
                                                onClick=${() => control(id, 'start')}>▶</button>
                                        <button class="btn btn-danger" title="Stop" onClick=${() => control(id, 'stop')}>■</button>
                                        <button class="btn" title="Parameters" onClick=${() => configure(id)}>⚙</button>
                                    </div></td>
                                </tr>`;
                        })}
                    </tbody>
                </table>
            </div>
            ${editing && html`
                <${Dialog} title=${`Configure: ${editing.id}`} onClose=${close}
                    footer=${html`<button class="btn" onClick=${close}>Cancel</button>
                                  <button class="btn btn-primary" onClick=${save}>Save</button>`}>
                    <div class="form">
                        <label class="field"><span>Parameters (JSON)</span>
                            <textarea class="config-editor" spellcheck="false" value=${editing.text}
                                      onInput=${(e) => setEditing({ ...editing, text: e.target.value })} />
                        </label>
                    </div>
                    ${editing.error && html`<div class="modal-error">${editing.error}</div>`}
                <//>`}
        </section>`;
}
