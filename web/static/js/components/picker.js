// A searchable instrument dropdown: type to filter, arrows and Enter to pick. It renders the
// first PICKER_LIMIT matches only, so a venue's worth of instruments stays quick.
import { html, useEffect, useMemo, useRef, useState } from '../core/html.js';
import { instruments } from '../core/instruments.js';

export const PICKER_LIMIT = 200;

// Re-renders the caller as the list changes.
export function useInstruments() {
    const [, setVersion] = useState(0);
    useEffect(() => instruments.subscribe(() => setVersion(v => v + 1)), []);
    return instruments;
}

export function Picker({ value, onPick, placeholder = 'Select instrument', autoFocus = false }) {
    const list = useInstruments();
    const [open, setOpen] = useState(false);
    const [query, setQuery] = useState('');
    const [active, setActive] = useState(0);
    const input = useRef(null);
    const items = useRef(null);

    const { names, total } = useMemo(() => (open ? list.match(query, PICKER_LIMIT) : { names: [], total: 0 }),
                                     [open, query, list.names]);

    useEffect(() => {
        if (autoFocus && !value) {
            input.current?.focus();
        }
    }, []);
    useEffect(() => {
        items.current?.children[active]?.scrollIntoView({ block: 'nearest' });
    }, [active]);

    const pick = (name) => {
        setOpen(false);
        setQuery('');
        input.current?.blur();
        if (name && name !== value) {
            onPick(name);
        }
    };
    const onKeyDown = (e) => {
        if (e.key === 'ArrowDown') {
            e.preventDefault();
            setOpen(true);
            setActive(i => Math.min(i + 1, Math.max(names.length - 1, 0)));
        } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            setActive(i => Math.max(i - 1, 0));
        } else if (e.key === 'Enter') {
            e.preventDefault();
            pick(names[active] ?? (list.has(query.trim()) ? query.trim() : null));
        } else if (e.key === 'Escape') {
            pick(null);
        }
    };

    const emptyText = !list.loaded ? 'Loading instruments…'
                    : list.names.length === 0 ? 'No instruments listed: brokers have defined none MANUAL takes'
                    : 'No match';
    return html`
        <div class="picker">
            <input ref=${input} type="text" spellcheck="false" autocomplete="off"
                   placeholder=${value || placeholder} value=${open ? query : (value ?? '')}
                   onFocus=${() => { setOpen(true); setQuery(''); setActive(0); }}
                   onBlur=${() => setOpen(false)}
                   onInput=${(e) => { setQuery(e.target.value); setActive(0); }}
                   onKeyDown=${onKeyDown} />
            ${open && html`
                <div class="picker-list" ref=${items}>
                    ${names.map((name, i) => html`
                        <div class=${`picker-item${i === active ? ' active' : ''}`}
                             onMouseDown=${(e) => { e.preventDefault(); pick(name); }}>${name}</div>`)}
                    ${names.length === 0 && html`<div class="picker-more">${emptyText}</div>`}
                    ${total > names.length && html`<div class="picker-more">${total - names.length} more: keep typing</div>`}
                </div>`}
        </div>`;
}
