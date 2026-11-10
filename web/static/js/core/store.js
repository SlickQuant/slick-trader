// localStorage (shared by this host's windows) and sessionStorage (one window's, kept across a
// reload) as JSON. Storage can be missing or refuse, as in a private window: reads fall back,
// writes are dropped.
function storage(get) {
    return {
        get(key, fallback = null) {
            try {
                const value = get().getItem(key);
                return value === null ? fallback : JSON.parse(value);
            } catch {
                return fallback;
            }
        },
        set(key, value) {
            try {
                get().setItem(key, JSON.stringify(value));
            } catch {
                // dropped: a convenience only
            }
        },
    };
}

export const local = storage(() => window.localStorage);
export const session = storage(() => window.sessionStorage);
