// P&L over time, summed across strategies, from the moment the window opens: the server keeps no
// history of it.
import { html, useEffect, useRef } from '../core/html.js';
import { api } from '../core/api.js';
import { useOnConnect, useWs } from '../core/connection.js';
import { loadScript } from '../core/load.js';
import { notify } from '../core/notify.js';

const MAX_POINTS = 300;

const SERIES = [
    ['Total P&L', '#3b82f6', 'total_pnl', true],
    ['Realized P&L', '#10b981', 'realized_pnl', false],
    ['Unrealized P&L', '#f59e0b', 'unrealized_pnl', false],
];

function createChart(canvas) {
    const grid = { color: 'rgba(42, 49, 66, 0.5)' };
    return new Chart(canvas, {
        type: 'line',
        data: {
            labels: [],
            datasets: SERIES.map(([label, color, , fill]) => ({
                label, data: [], borderColor: color, backgroundColor: `${color}1a`,
                borderWidth: 2, fill, tension: 0.3, pointRadius: 0, pointHoverRadius: 5,
            })),
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            interaction: { mode: 'index', intersect: false },
            plugins: {
                legend: { labels: { color: '#e4e6eb', usePointStyle: true } },
                tooltip: {
                    callbacks: { label: (c) => `${c.dataset.label}: $${c.parsed.y?.toFixed(2)}` },
                },
            },
            scales: {
                x: { grid, ticks: { color: '#8b92a7', maxRotation: 0, autoSkipPadding: 20 } },
                y: { position: 'right', grid, ticks: { color: '#8b92a7', callback: (v) => `$${v.toFixed(0)}` } },
            },
        },
    });
}

export default function Pnl() {
    const canvas = useRef(null);
    const chart = useRef(null);

    useEffect(() => {
        let alive = true;
        loadScript('lib/chart.umd.js').then(() => {
            if (alive) {
                chart.current = createChart(canvas.current);
            }
        }).catch((error) => notify(error.message, 'error'));
        return () => {
            alive = false;
            chart.current?.destroy();
        };
    }, []);

    const add = (byStrategy) => {
        const c = chart.current;
        if (!c) {
            return;
        }
        const strategies = Object.values(byStrategy ?? {});
        c.data.labels.push(new Date().toLocaleTimeString('en-US', { hour12: false }));
        SERIES.forEach(([, , key], i) => c.data.datasets[i].data.push(strategies.reduce((sum, p) => sum + (p[key] || 0), 0)));
        if (c.data.labels.length > MAX_POINTS) {
            c.data.labels.shift();
            c.data.datasets.forEach((d) => d.data.shift());
        }
        c.update('none');
    };

    useOnConnect(async () => {
        try {
            add(await api.getAllPnL());
        } catch (error) {
            console.error('[P&L] Failed to load:', error);
        }
    });
    useWs('pnl_update', (data) => add(data.pnl || data));

    return html`
        <div class="tool-window">
            <div class="chart-host"><canvas ref=${canvas}></canvas></div>
        </div>`;
}
