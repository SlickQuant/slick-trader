// Each tool opens in a popup window of its own: no tabs, menus or bookmarks. Windows are found
// by name, so any window, the main one reloaded too, focuses one already open instead of opening
// another. Ladders are the exception: each click opens one more.
import { local } from './store.js';
import { on, post, startDashboard } from './bus.js';
import { notify } from './notify.js';

export const TOOLS = {
    ladder:    { title: 'Ladder',    width: 360, height: 720, multi: true },
    order:     { title: 'Ticket',    width: 380, height: 560 },
    orders:    { title: 'Orders',    width: 960, height: 420 },
    fills:     { title: 'Fills',     width: 1000, height: 420 },
    positions: { title: 'Positions', width: 720, height: 360 },
    pnl:       { title: 'P&L',       width: 820, height: 440 },
    log:       { title: 'Log',       width: 760, height: 420 },
};

const MAX_LADDERS = 64;
const MAIN_NAME = 'st-main';
const opened = new Map();   // window name -> the window this one opened by it, or one that said it is open
const BLOCKED = Symbol('blocked');

// How long tool windows get to say where they are, and how often closeTools() asks again.
const LIST_MS = 300;
const CLOSE_TRIES = 6;
const CLOSE_GRACE_MS = 100;   // a window gone quiet may still hold its name a moment
const MAIN_CHECKS = 8;
const MAIN_CHECK_MS = 250;    // a main window says it goes before it shows as closed
const MAIN_POLL_MS = 1000;    // how often a tool window looks for one that can't say so
const SWITCH_MS = 500;        // a focus this close to another window's blur came from inside the dashboard

const geometryKey = (name) => `st.win.${name}`;

// Every screen, once the page may place windows on any; until then only this window's.
let screenDetails = null;
const screens = () => screenDetails?.screens ?? [window.screen];
const onScreen = (s, at) => {
    const left = s.availLeft ?? 0;
    const top = s.availTop ?? 0;
    return at.left >= left && at.left < left + s.availWidth && at.top >= top && at.top < top + s.availHeight;
};
const onAScreen = (at) => screens().some((s) => onScreen(s, at));

// Where a window opens: inside the screen its corner is on, when this page knows that one, else
// inside this window's screen. Moved and shrunk to fit, a layout saved on a screen since gone, or
// bigger than the one it lands on, stays in sight.
function place(at) {
    const s = screens().find((one) => onScreen(one, at)) ?? window.screen;
    const { availLeft: x = 0, availTop: y = 0, availWidth, availHeight } = s;
    if (!availWidth || !availHeight) {
        return at;
    }
    const width = Math.min(at.width, availWidth);
    const height = Math.min(at.height, availHeight);
    return {
        left: Math.min(Math.max(at.left, x), x + availWidth - width),
        top: Math.min(Math.max(at.top, y), y + availHeight - height),
        width,
        height,
    };
}

// Lets this page place windows on every screen: Chrome and Edge keep a popup on the opener's
// screen otherwise. Once allowed, it reads them again each time: a screen may have come or gone.
// The first time it asks only when `ask`: a click may, but a start has no click behind the
// prompt, which the browser then refuses or keeps every window waiting on.
async function knowScreens(ask) {
    if (!window.getScreenDetails) {
        return;
    }
    try {
        if (!screenDetails && !ask &&
            (await navigator.permissions?.query({ name: 'window-management' }))?.state !== 'granted') {
            return;
        }
        screenDetails = await window.getScreenDetails();
    } catch {
        screenDetails = null;   // refused, since revoked, or no such permission here: this screen only
    }
}

// `at` is where a workspace had it; else it opens where it was last.
function features(name, tool, at) {
    const saved = at ?? local.get(geometryKey(name));
    if (!saved) {
        return `popup=yes,width=${tool.width},height=${tool.height}`;
    }
    const p = place({ width: tool.width, height: tool.height, ...saved });
    return `popup=yes,width=${p.width},height=${p.height},left=${p.left},top=${p.top}`;
}

function toolUrl(tool, params) {
    return new URL(`tool.html?${new URLSearchParams({ t: tool, ...params })}`, location.href).href;
}

// The window named `name`: `fresh` when it was just opened, blank, for the caller to load.
// null when the browser blocked the popup.
function find(name, tool, open, at) {
    const known = opened.get(name);
    if (known && !known.closed) {
        return { win: known, fresh: false };
    }
    // An empty URL opens nothing new where a window of that name is open: it is handed back as is.
    const win = open('', name, features(name, tool, at));
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

// openTool() without the blocked popup's toast: the window, null, or BLOCKED.
function launch(tool, params, open, at) {
    const def = Object.hasOwn(TOOLS, tool) ? TOOLS[tool] : null;   // not 'constructor'
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
            const found = find(name, def, open, at);
            if (!found) {
                return BLOCKED;
            }
            if (found.fresh) {
                found.win.location.replace(toolUrl(tool, params));
                return found.win;
            }
        }
        notify(`At most ${MAX_LADDERS} ladders`, 'warning');
        return null;
    }
    const found = find(`st-${tool}`, def, open, at);
    if (!found) {
        return BLOCKED;
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

// Opens `tool`, or focuses its window and hands it `params`. Returns the window, or null.
// `at` ({left, top, width, height}) places a new one there.
export function openTool(tool, params = {}, open = window.open, at = undefined) {
    const win = launch(tool, params, open, at);
    return win === BLOCKED ? blocked() : win;
}

function blocked() {
    notify('The browser blocked the window: allow popups for this site', 'error');
    return null;
}

// What this tool window opens with again in a workspace, as URL params: a ladder's instrument and
// settings.
let windowParams = {};
export function setWindowParams(params) {
    windowParams = params;
}

// In a tool window: keeps where it is and how big, for the next time it opens, and answers the
// main window: where it is for a workspace, and closing for the next one.
export function registerToolWindow(tool) {
    const name = window.name;
    if (!name.startsWith('st-')) {
        return;
    }
    const geometry = () => ({
        left: window.screenX, top: window.screenY, width: window.innerWidth, height: window.innerHeight,
    });
    const save = () => local.set(geometryKey(name), geometry());
    let timer = null;
    window.addEventListener('resize', () => {
        clearTimeout(timer);
        timer = setTimeout(save, 300);
    });
    window.addEventListener('pagehide', save);
    on('layout-query', ({ id }) => post('layout-report', { id, window: { name, tool, ...geometry(), params: windowParams } }));
    on('workspace-close', () => window.close());
    // It closes with the main window that opened it. A reload says it goes too, but keeps the window.
    // A main window on the browser's error page, as when it reloaded while the engine was down, runs
    // nothing to say it goes: it is looked at each second too.
    const main = window.opener;
    if (main) {
        const closeWithMain = () => main.closed && window.close();
        on('main-closing', async () => {
            for (let i = 0; i < MAIN_CHECKS && !main.closed; ++i) {
                await settle(MAIN_CHECK_MS);
            }
            closeWithMain();
        });
        setInterval(closeWithMain, MAIN_POLL_MS);
        // The main window keeps every tool window, for one raising the rest. A reload forgets them.
        const join = () => {
            try {
                main.stTools?.set(name, window);
            } catch {
                // the main window on another page, as the browser's error one
            }
        };
        join();
        on('main-ready', join);
        raiseTogether(main);
    }
}

// In the main window, before it posts anything: a dashboard of its own, whose tool windows close
// with it.
export function registerMainWindow() {
    startDashboard();
    window.name = MAIN_NAME;
    window.stTools = opened;
    window.stGrouped = new URLSearchParams(location.search).has('grouped');
    window.addEventListener('pagehide', () => post('main-closing'));
    knowScreens(false);   // a window the toolbar opens goes back on the screen it was on
    raiseTogether(window);
}

// A window brought forward from another app brings the rest of the dashboard along. A page may
// raise windows only on a user's input: the first click or key in it, once done, so a click still
// lands. Moving between the dashboard's own windows raises none: one's focus comes with another's blur.
function raiseTogether(main) {
    const frame = document.createElement('iframe');
    frame.hidden = true;
    document.body.append(frame);
    let focusAt = 0;
    let awayAt = 0;   // when another of the dashboard's windows last lost focus, or this one raised them
    on('window-away', ({ at }) => {
        awayAt = Math.max(awayAt, at);
    });
    window.addEventListener('focus', () => {
        focusAt = Date.now();
    });
    window.addEventListener('blur', () => {
        focusAt = 0;
        post('window-away', { at: Date.now() });
    });
    const input = () => {
        if (!focusAt || Math.abs(focusAt - awayAt) < SWITCH_MS || navigator.userActivation?.isActive === false) {
            return;
        }
        focusAt = 0;
        awayAt = Date.now();
        post('window-away', { at: awayAt });
        raise(main, frame.contentWindow);
    };
    window.addEventListener('pointerup', input, true);
    window.addEventListener('keyup', input, true);
}

// `name` while `win` is open under it on a dashboard page, else '': opening a name no window holds
// opens a blank one.
function nameOf(win, name) {
    try {
        return !win.closed && win.name === name ? name : '';
    } catch {
        return '';   // on the browser's error page
    }
}

// Raises the dashboard's other windows by name, then this one. A window opened by name takes the
// caller for its opener, so each gets the main window back. None may raise itself, but its hidden
// frame shares the input, and may once it is the window's opener. The app slick-trader-web.bat
// adds opens the main window `?grouped`: it keeps them together itself, on any way to the front.
function raise(main, frame) {
    const own = main === window;
    let tools;
    try {
        if (main.stGrouped) {
            return;
        }
        tools = [...main.stTools].map(([name, win]) => (name === window.name ? '' : nameOf(win, name))).filter(Boolean);
    } catch {
        return;   // no main window, or one on the browser's error page
    }
    if (!nameOf(main, MAIN_NAME) || (own && !tools.length)) {
        return;
    }
    for (const name of tools) {
        window.open('', name);
    }
    if (!own) {
        window.open('', MAIN_NAME);
        main.opener = null;
        for (const name of tools) {
            main.open('', name);
        }
    }
    frame.open('', window.name);
    new frame.Function('win', 'win.focus()')(window);
    if (own) {
        window.opener = null;
    } else {
        main.open('', window.name);
    }
}

const settle = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The tool windows open, as a workspace keeps them: { name, tool, left, top, width, height,
// params }, by name, so ladders reopen in their slots' order.
export async function listOpenTools(ms = LIST_MS) {
    const id = `${Date.now()}-${Math.random()}`;
    const found = new Map();
    const stop = on('layout-report', (m) => m.id === id && found.set(m.window.name, m.window));
    post('layout-query', { id });
    await settle(ms);
    stop();
    return [...found.values()].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
}

// Closes every tool window, and resolves once none answers: a name still held can't open again.
export async function closeTools(ms = LIST_MS) {
    post('workspace-close');
    for (const win of opened.values()) {
        try {
            win.close();
        } catch {
            // gone already
        }
    }
    for (let i = 0; i < CLOSE_TRIES; ++i) {
        if ((await listOpenTools(ms)).length === 0) {
            await settle(CLOSE_GRACE_MS);
            return;
        }
        post('workspace-close');
    }
}

// Opens a workspace's windows where each was, with what each showed, asking first to place them on
// other screens when `ask`. Returns how many opened, the tools of those skipped as unknown here,
// and the windows the browser blocked from the first on: unless the site may open pop-ups, a click
// opens one window and a start with no click none.
export async function openLayout(windows, open = window.open, ask = true) {
    if (screenDetails || !windows.every(onAScreen)) {
        await knowScreens(ask);
    }
    let opened = 0;
    const skipped = [];
    for (let i = 0; i < windows.length; ++i) {
        const w = windows[i];
        if (!Object.hasOwn(TOOLS, w.tool)) {
            skipped.push(w.tool);
            continue;
        }
        const win = launch(w.tool, w.params ?? {}, open, w);
        if (win === BLOCKED) {
            return { opened, skipped, blocked: windows.slice(i) };
        }
        opened += win ? 1 : 0;
    }
    return { opened, skipped, blocked: [] };
}
