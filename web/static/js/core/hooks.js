// Small hooks every view uses.
import { useEffect, useMemo, useRef } from './html.js';

// Calls fn() every `ms` while mounted.
export function useInterval(fn, ms) {
    const latest = useRef(fn);
    latest.current = fn;
    useEffect(() => {
        const id = setInterval(() => latest.current(), ms);
        return () => clearInterval(id);
    }, [ms]);
}

// A function that calls fn() once, `ms` after the first of a burst of calls: a burst of order
// updates reloads the orders once.
export function useCoalesced(fn, ms) {
    const latest = useRef(fn);
    latest.current = fn;
    const timer = useRef(null);
    useEffect(() => () => clearTimeout(timer.current), []);
    return useMemo(() => () => {
        if (timer.current === null) {
            timer.current = setTimeout(() => {
                timer.current = null;
                latest.current();
            }, ms);
        }
    }, [ms]);
}

// Sets document.title while mounted, and as it changes.
export function useTitle(title) {
    useEffect(() => {
        document.title = title;
    }, [title]);
}
