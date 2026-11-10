// Each tool opens in a popup window of its own: no tabs, menus or bookmarks. Windows are found
// by name, so any window, the main one reloaded too, focuses one already open instead of opening
// another. Ladders are the exception: each click opens one more.
import { local } from './store.js';
import { post } from './bus.js';
import { notify } from './notify.js';

export const TOOLS = {
    ladder:    { title: 'Ladder',    width: 360, height: 720, multi: true },
    order:     { title: 'Order',     width: 380, height: 560 },
    orders:    { title: 'Orders',    width: 960, height: 420 },
    fills:     { title: 'Fills',     width: 1000, height: 420 },
    positions: { title: 'Positions', width: 720, height: 360 },
    pnl:       { title: 'P&L',       width: 820, height: 440 },
    log:       { title: 'Log',       width: 760, height: 420 },
};

const MAX_LADDERS = 64;
const opened = new Map();   // window name -> the window this one opened by it

const geometryKey = (name) => `st.win.${name}`;

function features(name, tool) {
    const saved = local.get(geometryKey(name));
    const width = saved?.width ?? tool.width;
    const height = saved?.height ?? tool.height;
    const at = saved ? `,left=${saved.left},top=${saved.top}` : '';
    return `popup=yes,width=${width},height=${height}${at}`;
}

function toolUrl(tool, params) {
    return new URL(`tool.html?${new URLSearchParams({ t: tool, ...params })}`, location.href).href;
}

// The window named `name`: `fresh` when it was just opened, blank, for the caller to load.
// null when the browser blocked the popup.
function find(name, tool, open = window.open) {
    const known = opened.get(name);
    if (known && !known.closed) {
        return { win: known, fresh: false };
    }
    // An empty URL opens nothing new where a window of that name is open: it is handed back as is.
    const win = open('', name, features(name, tool));
    if (!win) {
        return null;
    }
    let fresh;
    try {
        fresh = win.location.href === 'about:blank';
    } catch {
        fresh = false;
    }
    opened.set(name, win);
    return { win, fresh };
}

// Opens `tool`, or focuses its window and hands it `params`. Returns the window, or null.
export function openTool(tool, params = {}, open = window.open) {
    const def = TOOLS[tool];
    if (!def) {
        return null;
    }
    if (def.multi) {
        // The lowest numbered slot no window holds. One this window did not open, as after a
        // reload, is found open and passed over.
        for (let n = 1; n <= MAX_LADDERS; ++n) {
            const name = `st-${tool}-${n}`;
            const known = opened.get(name);
            if (known && !known.closed) {
                continue;
            }
            const found = find(name, def, open);
            if (!found) {
                return blocked();
            }
            if (found.fresh) {
                found.win.location.replace(toolUrl(tool, params));
                return found.win;
            }
        }
        notify(`At most ${MAX_LADDERS} ladders`, 'warning');
        return null;
    }
    const found = find(`st-${tool}`, def, open);
    if (!found) {
        return blocked();
    }
    if (found.fresh) {
        found.win.location.replace(toolUrl(tool, params));
    } else {
        found.win.focus();
        if (Object.keys(params).length) {
            post('tool-params', { tool, params });
        }
    }
    return found.win;
}

function blocked() {
    notify('The browser blocked the window: allow popups for this site', 'error');
    return null;
}

// In a tool window: keeps where it is and how big, for the next time it opens.
export function trackGeometry() {
    const name = window.name;
    if (!name.startsWith('st-')) {
        return;
    }
    const save = () => local.set(geometryKey(name), {
        left: window.screenX, top: window.screenY, width: window.innerWidth, height: window.innerHeight,
    });
    let timer = null;
    window.addEventListener('resize', () => {
        clearTimeout(timer);
        timer = setTimeout(save, 300);
    });
    window.addEventListener('pagehide', save);
}
