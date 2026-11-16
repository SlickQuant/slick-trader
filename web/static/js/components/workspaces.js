// The toolbar's workspace menu: open a saved set of tool windows, each where it was and showing what
// it showed, or save the ones open now under a name. The engine keeps them. As the main window
// starts it opens the last used, unless tool windows are up already: it was reloaded under them.
import { html, useCallback, useEffect, useRef, useState } from '../core/html.js';
import { api } from '../core/api.js';
import { useOnConnect } from '../core/connection.js';
import { Dialog, notify } from '../core/notify.js';
import { closeTools, listOpenTools, openLayout } from '../core/windows.js';
import { Icon } from './icons.js';

export const DEFAULT_WORKSPACE = 'Default';

// Default first, then by name.
const byName = (a, b) => (b === DEFAULT_WORKSPACE) - (a === DEFAULT_WORKSPACE) || a.localeCompare(b);

// Where this browser lets a site open pop-ups without a click each.
const POPUP_SETTINGS = `${navigator.userAgent.includes('Edg/') ? 'edge' : 'chrome'}://settings/content/popups`;

export function Workspaces({ log }) {
    const [workspaces, setWorkspaces] = useState({});
    const [current, setCurrent] = useState(DEFAULT_WORKSPACE);
    const [menu, setMenu] = useState(false);
    const [naming, setNaming] = useState(null);   // Save as: { name, error }
    const [blocked, setBlocked] = useState(null);   // windows the browser kept shut: { name, total, windows }
    const [busy, setBusy] = useState(false);
    const inFlight = useRef(false);   // busy as of now: `busy` shows only from the next render
    const started = useRef(false);
    const root = useRef(null);
    const nameInput = useRef(null);

    const load = useCallback(async () => {
        const doc = await api.getWorkspaces();
        setWorkspaces(doc.workspaces ?? {});
        return doc;
    }, []);

    // Opens `windows` of workspace `name`, counting on from `so` when they are what the browser
    // blocked before. Those it blocks again wait in a dialog: each click there lets one more open.
    // `ask` is whether a click is behind it, to ask to place windows on other screens.
    const openWindows = async (name, windows, { so = { total: windows.length, opened: 0, skipped: [] }, ask = true } = {}) => {
        const result = await openLayout(windows, undefined, ask);
        const now = { name, total: so.total, opened: so.opened + result.opened,
                      skipped: [...so.skipped, ...result.skipped], windows: result.blocked };
        setBlocked(now.windows.length ? now : null);
        return now;
    };
    const logOpened = ({ name, total, opened, skipped, windows: left }) => {
        const unknown = skipped.length ? `, no tool here for ${skipped.join(', ')}` : '';
        log(left.length ? `Workspace ${name}: ${opened} of ${total} windows open, the browser blocked the rest${unknown}`
            : skipped.length ? `Workspace ${name} opened: ${opened} of ${total} windows${unknown}`
            : `Workspace ${name} opened: ${opened} window${opened === 1 ? '' : 's'}`,
            left.length || skipped.length ? 'warning' : 'info');
    };

    useOnConnect(async () => {
        try {
            const doc = await load();
            if (started.current) {
                return;
            }
            started.current = true;
            setCurrent(doc.last);
            const windows = doc.workspaces[doc.last]?.windows ?? [];
            if (windows.length > 0 && (await listOpenTools()).length === 0) {
                await run('Open workspace', async () => logOpened(await openWindows(doc.last, windows, { ask: false })));
            }
        } catch (error) {
            log(`Failed to load workspaces: ${error.message}`, 'error');
        }
    });

    // The menu closes on a click anywhere else, or Escape.
    useEffect(() => {
        if (!menu) {
            return undefined;
        }
        const onDown = (e) => root.current?.contains(e.target) || setMenu(false);
        const onKey = (e) => e.key === 'Escape' && setMenu(false);
        document.addEventListener('mousedown', onDown);
        document.addEventListener('keydown', onKey);
        return () => {
            document.removeEventListener('mousedown', onDown);
            document.removeEventListener('keydown', onKey);
        };
    }, [menu]);

    // One at a time: what starts while another runs is dropped.
    const run = async (what, fn) => {
        if (inFlight.current) {
            return;
        }
        inFlight.current = true;
        setMenu(false);
        setBusy(true);
        try {
            await fn();
        } catch (error) {
            notify(`${what} failed: ${error.message}`, 'error');
            log(`${what} failed: ${error.message}`, 'error');
        } finally {
            inFlight.current = false;
            setBusy(false);
        }
    };

    // Its windows replace the ones open. Read again first: another browser may have saved it since.
    const choose = (name) => run('Open workspace', async () => {
        const doc = await load();
        if (!Object.hasOwn(doc.workspaces, name)) {
            throw new Error(`${name} was deleted`);
        }
        await closeTools();
        logOpened(await openWindows(name, doc.workspaces[name]?.windows ?? []));
        setCurrent(name);
        await api.useWorkspace(name);
    });
    // The log hears after each click, counting from the start.
    const openMore = () => run('Open workspace', async () =>
        logOpened(await openWindows(blocked.name, blocked.windows, { so: blocked })));
    const closeBlocked = useCallback(() => setBlocked(null), []);

    const saveTo = (name) => run('Save workspace', async () => {
        const windows = await listOpenTools();
        await api.saveWorkspace(name, windows);
        setCurrent(name);
        await load();
        const msg = `Workspace ${name} saved: ${windows.length} window${windows.length === 1 ? '' : 's'}`;
        notify(msg, 'success');
        log(msg, 'success');
    });

    // A new name makes a workspace; one there already is replaced, once confirmed.
    const saveAs = () => {
        const name = naming.name.trim();
        if (inFlight.current) {
            return;
        }
        if (!name) {
            setNaming({ ...naming, error: 'Name the workspace' });
            return;
        }
        if (name !== current && Object.hasOwn(workspaces, name) && !confirm(`Replace workspace ${name}?`)) {
            return;
        }
        setNaming(null);
        saveTo(name);
    };
    const closeNaming = useCallback(() => setNaming(null), []);
    const isNaming = naming !== null;
    useEffect(() => {
        if (isNaming) {
            nameInput.current?.focus();
        }
    }, [isNaming]);

    // Its windows stay open, now under Default.
    const remove = () => {
        const name = current;
        if (inFlight.current || name === DEFAULT_WORKSPACE || !confirm(`Delete workspace ${name}?`)) {
            return;
        }
        run('Delete workspace', async () => {
            await api.deleteWorkspace(name);
            setCurrent(DEFAULT_WORKSPACE);
            await load();
            log(`Workspace ${name} deleted`, 'info');
        });
    };

    const names = Object.keys(workspaces).sort(byName);
    return html`
        <div class="workspaces" ref=${root}>
            <button class="tool-btn" title="Workspaces: saved window layouts" disabled=${busy}
                    onClick=${() => setMenu(!menu)}>
                <${Icon} name="workspace" />
                <span>${current} ▾</span>
            </button>
            ${menu && html`<div class="menu">
                ${names.map(name => html`
                    <button class="menu-item" key=${name} disabled=${busy} onClick=${() => choose(name)} title=${`Open ${name}`}>
                        <span class="menu-check">${name === current ? '✓' : ''}</span>${name}
                    </button>`)}
                <div class="menu-sep"></div>
                <button class="menu-item" disabled=${busy} onClick=${() => saveTo(current)}>
                    <span class="menu-check"></span>Save to ${current}
                </button>
                <button class="menu-item" disabled=${busy} onClick=${() => { setMenu(false); setNaming({ name: '', error: '' }); }}>
                    <span class="menu-check"></span>Save as…
                </button>
                <button class="menu-item" disabled=${busy || current === DEFAULT_WORKSPACE} onClick=${remove}>
                    <span class="menu-check"></span>Delete ${current}
                </button>
            </div>`}
            ${naming && html`
                <${Dialog} title="Save workspace as" onClose=${closeNaming}
                    footer=${html`<button class="btn" onClick=${closeNaming}>Cancel</button>
                                  <button class="btn btn-primary" disabled=${busy} onClick=${saveAs}>Save</button>`}>
                    <div class="form">
                        <label class="field"><span>Name: a new one, or one to replace</span>
                            <input list="workspace-names" maxlength="64" value=${naming.name} ref=${nameInput}
                                   onInput=${(e) => setNaming({ name: e.target.value, error: '' })}
                                   onKeyDown=${(e) => e.key === 'Enter' && saveAs()} />
                        </label>
                        <datalist id="workspace-names">
                            ${names.map(name => html`<option value=${name} />`)}
                        </datalist>
                    </div>
                    ${naming.error && html`<div class="modal-error">${naming.error}</div>`}
                <//>`}
            ${blocked && html`
                <${Dialog} title=${`Open ${blocked.name}`} onClose=${closeBlocked}
                    footer=${html`<button class="btn" onClick=${closeBlocked}>Cancel</button>
                                  <button class="btn btn-primary" disabled=${busy} onClick=${openMore}>
                                      Open next (${blocked.windows.length} left)</button>`}>
                    <div class="form">
                        <p>${blocked.opened} of ${blocked.total} windows open. The browser
                           lets each click open one, unless this site may open pop-ups.</p>
                        <p>To open a workspace in one go, allow pop-ups for ${location.host} at <code>${POPUP_SETTINGS}</code>.</p>
                    </div>
                <//>`}
        </div>`;
}
