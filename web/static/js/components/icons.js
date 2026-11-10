// Toolbar glyphs, drawn with the current text color.
import { html } from '../core/html.js';

const PATHS = {
    ladder: 'M6 3v18M18 3v18M6 7h12M6 11h12M6 15h12M6 19h12',
    order: 'M5 4h14v16H5zM8 9h8M8 13h8M8 17h5',
    orders: 'M4 6h16M4 12h16M4 18h16M2 6h0M2 12h0M2 18h0',
    fills: 'M4 12l5 5L20 6',
    positions: 'M4 20V10M10 20V4M16 20v-7M22 20H2',
    pnl: 'M3 17l6-6 4 4 8-8M15 7h6v6',
    log: 'M4 4h16v16H4zM8 8h8M8 12h8M8 16h4',
};

export function Icon({ name }) {
    return html`<svg viewBox="0 0 24 24" aria-hidden="true"><path d=${PATHS[name]} /></svg>`;
}
