# SlickTrader Web Dashboard

The engine serves the dashboard at `http://localhost:<web_server_port>` (3000 in `config/config.json`).

## Layout

The main window has a toolbar on top: Ladder, Order, Orders, Fills, Positions, P&L and Log. Under it
sit broker status, P&L totals and strategy control. Each button opens its tool in a popup window
of its own, with no tabs, menus or bookmark bar. Ladder opens a new window per click. The other
buttons focus their window if it's already open. Each window remembers its size and position.
See [FEATURES.md](FEATURES.md) for what each window does.

Allow popups for the dashboard's host. Chrome and Edge show a thin read-only address strip on a
popup, and no page can remove it. Starting the main window as an app drops its own address bar and
tabs: `msedge --app=http://<host>:3000` or `chrome --app=http://<host>:3000`. Skip `--kiosk`, which
locks the main window to full screen.

## Ladder market data

A ladder lists the instruments brokers define, narrowed by MANUAL's `instruments` parameter
(symbols, or prefixes ending in `*`). With no parameter it lists all of them, and with `[]` none.
It also lists what other strategies subscribe. Coinbase and Hyperliquid define their products when
they start. Coinbase reads them from `market_data.rest_url`.

Nothing streams until a ladder picks an instrument. MANUAL then subscribes its level 2 book, and
unsubscribes it `web_server_ladder_release_ms` (5000) after the last ladder on it closes or
switches away. That never drops a book a strategy also subscribed. If a strategy already streams
the book (LEVEL2 or ORDER_BOOK), the ladder uses that stream and MANUAL subscribes nothing. A
strategy's quotes or trades carry no book, so they don't count.

## Files

```
index.html, tool.html   main window, and the shell every tool window loads (tool.html?t=<tool>)
css/main.css            the one stylesheet
lib/                    preact 11.0.2, htm 3.1.1, chart.js 4.4.1 (vendored)
js/main.js              toolbar, summary, the log the Log window shows
js/tool.js              mounts one tool, picked from a fixed registry
js/core/                api, websocket, connection hooks, windows, bus, instruments, log
js/components/          picker, strategy control, icons
js/tools/               ladder, order, orders, fills, positions, pnl, log
js/slick-ladder.js      the ladder canvas (@slickquant/slick-ladder)
```

There's no build step. Pages load ES modules through an import map. To update a library, run
`npm install` and copy its dist file into `lib/` under a `.js` name. The server sends only `.js`
files as JavaScript, and browsers refuse to run a module served as anything else.

Every window opens its own WebSocket. Windows talk to each other over a `BroadcastChannel`: tool
windows send log entries and ticket prefills, and the main window relays the log.

## API

REST: `GET /api/health` (status and engine version), `/api/brokers`, `/api/strategies`, `/api/positions`, `/api/pnl`,
`/api/orders`, `/api/fills` (the last 1,000 fills, oldest first), `/api/ladder-config`.
`POST /api/orders`, `PUT`/`DELETE /api/orders/:strategy/:order_id`, `DELETE /api/orders/cancel-all`,
`POST /api/strategies/:id/control`, `GET`/`PUT /api/strategies/:id/config`. See
[CONTROLS_GUIDE.md](CONTROLS_GUIDE.md).

WebSocket, client to server:

- `{"type":"subscribe","channel":"instruments"}`: the whole list right away, then additions.
- `{"type":"subscribe","channel":"level2","instrument":"ZNZ6"}` and `unsubscribe`.
- `{"type":"ping"}`.

Server to client:

- `welcome`, `pong`.
- `instruments` / `instruments_added`: `{"instruments":{"ZNZ6":[<broker id>,<tick>]}}`. The tick
  is 0 when the broker gives none.
- `level2`: a snapshot when `is_snapshot` is set, then changes.
- `order_update` with `event` `ack`, `fill`, `leg_fill`, `cancel` or `reject`. Fills carry `seq`,
  `strategy_id`, `fill_id`, `fee` and `timestamp_ns`.
- `pnl_update`, `broker_status`.
- `subscription_error`: the server couldn't pass a ladder's subscribe on to MANUAL.

## Config

| Key | Default | |
|---|---|---|
| `web_server_port` | 8080 | |
| `web_server_static_path` | `web/static` | |
| `web_server_cors` | true | |
| `web_server_ladder_release_ms` | 5000 | grace before a ladder's instrument is unsubscribed |
| `ladder_config` | `{"default_qty":0.01}` | `default_qty`, and per instrument `default_qty` and `default_broker_id` |
| broker `market_data.rest_url` (Coinbase) | `https://api.coinbase.com` | where products are listed |

There's no authentication. Keep the port on localhost, or put it behind a proxy that adds TLS and auth.
