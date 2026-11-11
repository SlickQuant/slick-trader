// The main window: the toolbar that opens each tool in a window of its own, and a summary of
// brokers, P&L and strategies. It keeps the system log, which the Log window shows.
import { html, render, useCallback, useEffect, useState } from './core/html.js';
import { api, colorClass, formatCurrency } from './core/api.js';
import { connect, useConnection, useOnConnect, useWs } from './core/connection.js';
import { useInterval } from './core/hooks.js';
import * as bus from './core/bus.js';
import { LogRing, formatOrderLog, orderLogLevel } from './core/log.js';
import { TOOLS, openTool } from './core/windows.js';
import { Icon } from './components/icons.js';
import { Strategies } from './components/strategies.js';

const PNL_POLL_MS = 5000;
const TOOLBAR = ['ladder', 'order', 'orders', 'fills', 'positions', 'pnl', 'log'];

const ring = new LogRing();

// The Log window says when it opens and closes; errors while it is closed show on its button.
let logOpen = false;
const errorsListeners = new Set();
let errorsSinceOpen = 0;
const setErrors = (n) => {
    errorsSinceOpen = n;
    errorsListeners.forEach(fn => fn(n));
};

ring.subscribe((entry) => {
    bus.post('log-entry', { entry });
    if (entry.level === 'error' && !logOpen) {
        setErrors(errorsSinceOpen + 1);
    }
});
bus.on('log', (m) => ring.add(m.message, m.level));
bus.on('log-sync', () => {
    logOpen = true;
    setErrors(0);
    bus.post('log-history', { entries: ring.entries });
});
bus.on('log-closed', () => { logOpen = false; });
bus.on('log-clear', () => ring.clear());

const log = (message, level) => ring.add(message, level);

function ConnectionDot() {
    const state = useConnection();
    const label = { connected: 'Connected', disconnected: 'Disconnected', connecting: 'Connecting…' }[state];
    return html`<div class="connection"><span class=${`dot ${state}`}></span>${label}</div>`;
}

function Toolbar() {
    const [errors, setErrorsState] = useState(errorsSinceOpen);
    useEffect(() => {
        errorsListeners.add(setErrorsState);
        return () => errorsListeners.delete(setErrorsState);
    }, []);
    // Asked on every connect: the engine that comes back may be a new version.
    const [version, setVersion] = useState('');
    useOnConnect(async () => {
        try {
            setVersion((await api.getHealth()).version ?? '');
        } catch {
            // keeps the last one known
        }
    });
    return html`
        <header class="toolbar">
            <div class="brand">SlickTrader</div>
            ${version && html`<span class="version" title="Engine version">v${version}</span>`}
            <nav class="tools">
                ${TOOLBAR.map(tool => html`
                    <button class="tool-btn" key=${tool} onClick=${() => openTool(tool)}
                            title=${TOOLS[tool].multi ? `New ${TOOLS[tool].title.toLowerCase()} window` : `Open ${TOOLS[tool].title}`}>
                        <${Icon} name=${tool} />
                        <span>${TOOLS[tool].title}</span>
                        ${tool === 'log' && errors > 0 && html`<span class="badge" title="Errors since the log was last open">${errors}</span>`}
                    </button>`)}
            </nav>
            <${ConnectionDot} />
        </header>`;
}

function BrokerStatus() {
    const [brokers, setBrokers] = useState(null);
    useOnConnect(async () => {
        try {
            setBrokers((await api.getBrokers()).brokers ?? []);
        } catch {
            setBrokers([]);
        }
    });
    useWs('broker_status', (data) => setBrokers(data.brokers ?? []));

    const dot = (c) => (!c.enabled ? 'disabled' : c.connected ? 'connected' : 'disconnected');
    const label = (name, c) => (!c.enabled ? `${name} (off)` : c.connected ? name : `${name} (disconnected)`);
    return html`
        <section class="panel">
            <div class="panel-header"><h3>Brokers</h3></div>
            <div class="panel-body brokers">
                ${brokers === null && html`<span class="muted">Loading…</span>`}
                ${brokers?.length === 0 && html`<span class="muted">No brokers configured</span>`}
                ${brokers?.map(b => html`
                    <div class="broker" key=${b.id}>
                        <span class="broker-name">${b.name}</span>
                        <span class="connector"><span class=${`dot ${dot(b.md)}`}></span>${label('MD', b.md)}</span>
                        <span class="connector"><span class=${`dot ${dot(b.order)}`}></span>${label('Order', b.order)}</span>
                    </div>`)}
            </div>
        </section>`;
}

function PnlTotals({ pnl }) {
    const totals = Object.values(pnl).reduce((t, p) => ({
        total: t.total + (p.total_pnl || 0),
        realized: t.realized + (p.realized_pnl || 0),
        unrealized: t.unrealized + (p.unrealized_pnl || 0),
    }), { total: 0, realized: 0, unrealized: 0 });
    const metric = (label, value) => html`
        <div class="panel metric">
            <div class="metric-label">${label}</div>
            <div class=${`metric-value ${colorClass(value)}`}>${formatCurrency(value)}</div>
        </div>`;
    return html`
        <div class="metrics">
            ${metric('Total P&L', totals.total)}
            ${metric('Realized P&L', totals.realized)}
            ${metric('Unrealized P&L', totals.unrealized)}
        </div>`;
}

function Main() {
    const [pnl, setPnl] = useState({});

    const loadPnl = useCallback(async () => {
        try {
            setPnl(await api.getAllPnL());
        } catch (error) {
            console.error('[Main] Failed to load P&L:', error);
        }
    }, []);
    useOnConnect(() => {
        log('WebSocket connected', 'success');
        loadPnl();
    });
    useInterval(loadPnl, PNL_POLL_MS);
    useWs('pnl_update', (data) => setPnl(data.pnl || data));
    useWs('disconnected', () => log('WebSocket disconnected', 'error'));
    useWs('reconnecting', (data) => log(`Reconnecting (attempt ${data.attempt})...`, 'warning'));
    useWs('welcome', (data) => log(data.message, 'info'));
    useWs('order_update', (data) => log(formatOrderLog(data), orderLogLevel(data)));

    return html`
        <div class="main-window">
            <${Toolbar} />
            <main class="summary">
                <${BrokerStatus} />
                <${PnlTotals} pnl=${pnl} />
                <${Strategies} pnl=${pnl} log=${log} />
            </main>
        </div>`;
}

log('Dashboard initialized', 'info');
bus.post('main-ready');
render(html`<${Main} />`, document.getElementById('app'));
connect();
