// The system log, as the main window keeps it: asked for as the window opens, then each entry
// as the main window adds it. It shows the log and adds nothing to it.
import { html, useEffect, useRef, useState } from '../core/html.js';
import { useTitle } from '../core/hooks.js';
import { LOG_CAPACITY, formatLogTime } from '../core/log.js';
import * as bus from '../core/bus.js';

const NO_MAIN_MS = 1000;   // how long the main window has to answer before it is taken for closed

export default function Log() {
    useTitle('Log');
    const [entries, setEntries] = useState([]);
    const [mainWindow, setMainWindow] = useState(true);
    const answered = useRef(false);
    const body = useRef(null);
    const follow = useRef(true);   // at the bottom: it stays there as entries come

    useEffect(() => {
        const heard = () => {
            answered.current = true;
            setMainWindow(true);
        };
        const stops = [
            bus.on('log-history', (m) => { heard(); setEntries(m.entries); }),
            bus.on('log-entry', (m) => {
                heard();
                setEntries((list) => list.concat(m.entry).slice(-LOG_CAPACITY));
            }),
        ];
        // Asked again of a main window reloaded since: its log starts over.
        stops.push(bus.on('main-ready', () => bus.post('log-sync')));
        bus.post('log-sync');
        const timer = setTimeout(() => setMainWindow(answered.current), NO_MAIN_MS);
        const closed = () => bus.post('log-closed');
        window.addEventListener('pagehide', closed);
        return () => {
            stops.forEach((stop) => stop());
            clearTimeout(timer);
            window.removeEventListener('pagehide', closed);
        };
    }, []);
    useEffect(() => {
        if (follow.current && body.current) {
            body.current.scrollTop = body.current.scrollHeight;
        }
    }, [entries]);

    const clear = () => {
        setEntries([]);
        bus.post('log-clear');
    };
    return html`
        <div class="tool-window">
            <div class="tool-bar">
                <span class="muted">${entries.length} entr${entries.length === 1 ? 'y' : 'ies'}</span>
                <span class="spacer"></span>
                <button class="btn" onClick=${clear}>Clear</button>
            </div>
            ${!mainWindow && entries.length === 0 && html`
                <div class="tool-note">The main window keeps the log: open it to see entries here.</div>`}
            <div class="tool-body log" ref=${body}
                 onScroll=${(e) => { const el = e.target; follow.current = el.scrollTop + el.clientHeight >= el.scrollHeight - 4; }}>
                ${entries.map((e, i) => html`
                    <div class=${`log-entry log-${e.level}`} key=${`${e.time}-${i}`}>
                        <span class="log-time">[${formatLogTime(e.time)}]</span>
                        <span class="log-message">${e.message}</span>
                    </div>`)}
            </div>
        </div>`;
}
