// The shell every tool window runs: tool.html?t=<tool>. It mounts that tool over the whole window,
// under the environment bar off production, with a socket of its own.
import { html, render } from './core/html.js';
import { connect } from './core/connection.js';
import { setTitle } from './core/hooks.js';
import { TOOLS, registerToolWindow } from './core/windows.js';
import { EnvBar } from './components/env-bar.js';

// Only these: the name in the URL never becomes an import path.
const REGISTRY = {
    ladder: () => import('./tools/ladder.js'),
    order: () => import('./tools/order.js'),
    orders: () => import('./tools/orders.js'),
    fills: () => import('./tools/fills.js'),
    positions: () => import('./tools/positions.js'),
    pnl: () => import('./tools/pnl.js'),
    log: () => import('./tools/log.js'),
};

const params = new URLSearchParams(location.search);
const tool = params.get('t');
const app = document.getElementById('app');

if (!Object.hasOwn(REGISTRY, tool)) {
    app.innerHTML = '<div class="tool-note">Unknown tool</div>';
} else {
    setTitle(TOOLS[tool].title);
    registerToolWindow(tool);
    const { default: Tool } = await REGISTRY[tool]();
    render(html`<div class="tool-shell"><${EnvBar} /><${Tool} params=${params} /></div>`, app);
    connect();
}
