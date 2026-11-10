// The instruments a ladder can show, as the server lists them: name -> [broker id, tick]. Kept
// sorted, with each name in capitals too, so a filter over a venue's worth stays a plain scan.
import { api } from './api.js';
import { ws, subscribeChannel } from './connection.js';

export class Instruments {
    constructor() {
        this.names = [];          // sorted
        this.upper = [];          // names[i] in capitals, for a case-blind match
        this.info = new Map();    // name -> { broker, tick }
        this.loaded = false;      // the whole list came
        this.config = null;       // the ladder config: default qty, per-instrument broker
        this.listeners = new Set();
    }

    // The whole list, sent on every subscribe: what a restarted server no longer lists goes.
    set(list) {
        this.info = new Map(Object.entries(list ?? {}).map(([name, [broker, tick]]) => [name, { broker, tick }]));
        this.names = Array.from(this.info.keys()).sort();
        this.upper = this.names.map(name => name.toUpperCase());
        this.loaded = true;
        this.emit();
    }

    // What is new since, or newly given a tick. Both in order: each new one goes before the first
    // listed one past it.
    add(list) {
        const added = [];
        for (const [name, [broker, tick]] of Object.entries(list ?? {})) {
            if (!this.info.has(name)) {
                added.push(name);
            }
            this.info.set(name, { broker, tick });
        }
        if (added.length) {
            added.sort();
            const merged = [];
            let next = 0;
            for (const name of added) {
                while (next < this.names.length && this.names[next] < name) {
                    merged.push(this.names[next++]);
                }
                merged.push(name);
            }
            this.names = merged.concat(this.names.slice(next));
            this.upper = this.names.map(name => name.toUpperCase());
        }
        this.emit();
    }

    has(name) { return this.info.has(name); }

    tickFor(name) { return this.info.get(name)?.tick || 0; }

    // The broker an order for `name` goes to: the one its own ladder config names, else the one
    // listing it. Undefined when neither is known.
    brokerFor(name) {
        return this.config?.per_instrument?.[name]?.default_broker_id ?? this.info.get(name)?.broker;
    }

    // The ladder config, with an instrument's own over the defaults.
    configFor(name) {
        const config = this.config ?? { default_qty: 0.01, default_strategy_id: '' };
        return { ...config, ...(config.per_instrument?.[name] ?? {}) };
    }

    // The first `limit` names holding `query`, any case, and how many hold it in all.
    match(query, limit = 200) {
        const q = query.trim().toUpperCase();
        const found = [];
        let total = 0;
        for (let i = 0; i < this.upper.length; ++i) {
            if (q === '' || this.upper[i].includes(q)) {
                if (found.length < limit) {
                    found.push(this.names[i]);
                }
                ++total;
            }
        }
        return { names: found, total };
    }

    subscribe(fn) {
        this.listeners.add(fn);
        return () => this.listeners.delete(fn);
    }

    emit() {
        for (const fn of this.listeners) {
            fn();
        }
    }
}

export const instruments = new Instruments();

let watching = false;

// Has this window's socket ask for the list, and keeps `instruments` in step with it.
export function watchInstruments() {
    if (watching) {
        return;
    }
    watching = true;
    ws.on('instruments', (data) => instruments.set(data.instruments));
    ws.on('instruments_added', (data) => instruments.add(data.instruments));
    subscribeChannel('instruments');
    loadLadderConfig();
}

export async function loadLadderConfig() {
    try {
        instruments.config = await api.getLadderConfig();
    } catch (error) {
        console.error('[Instruments] Failed to load ladder config:', error);
        instruments.config = { default_qty: 0.01, default_strategy_id: '', per_instrument: {} };
    }
    instruments.emit();
}
