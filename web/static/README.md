# SlickTrader Web Dashboard

The engine serves the dashboard at `http://localhost:<web_server_port>` (3000 in `config/config.json`).

## Layout

The main window has a toolbar on top: Ladder, Ticket, Orders, Fills, Positions, P&L and Log, then
the Workspaces menu. Under it sit Venues (each broker's connections), P&L totals and strategy
control. Each button opens its tool in a popup window of its own, with no tabs, menus or bookmark
bar. Ladder opens a new window per click. The other buttons focus their window if it's already
open. Each window remembers its size and position. Closing the main window closes the tool windows
it opened, and reloading it keeps them. See [FEATURES.md](FEATURES.md) for what each window does.

Each main window is a dashboard of its own. Two open on one host, in tabs or windows, keep their
tool windows and logs apart, and a workspace saves and replaces only its own dashboard's windows.
The saved workspaces, and which one was last used, live in the engine, so every dashboard shares
them. If a main window dies without closing, as in a crash, its tool windows belong to no
dashboard any more and have to be closed by hand.

The Workspaces menu saves the open tool windows under a name, with each one's position and size
and a ladder's instrument. Opening a workspace closes the open tool windows and opens its own.
`Default` always exists and can't be deleted. When the main window starts and no tool window
answers, it opens the last used workspace, so reloading the main window alone changes nothing.
The main window isn't part of a workspace, since a page can't move a window the user opened.
Chrome and Edge keep popups on the main window's screen unless the site may manage windows.
Opening a workspace with a window on another screen asks for that permission. A start doesn't
ask, because the browser refuses a request with no click behind it. Each window opens inside one
screen, moved and shrunk to fit, and one saved on a screen that's gone, as after undocking, opens
on the main window's. A tool the dashboard no longer has is skipped, and the log says which.
Workspace names are trimmed, so "Morning " is Morning.

Allow popups for the dashboard's host (`chrome://settings/content/popups` or
`edge://settings/content/popups`). Without that, each click opens one window and a start with no
click opens none, so a workspace opens its first window and a dialog opens the rest one click at a
time.

On Windows the engine can allow them itself when the browser runs on the same PC, as the same
user. `"web_server_allow_popups": {"ip": "192.168.1.20", "port": 3000}` in `web_service` (ip
defaults to localhost, port to the server's) adds `http://<ip>:<port>` to the HKCU
`PopupsAllowedForUrls` policy of Edge and Chrome as the engine starts. Use the address you type in
the browser. The browser applies it on restart, or on Reload policies at `edge://policy`. The entry
stays after you remove the setting, so delete it under
`HKCU\Software\Policies\Microsoft\Edge\PopupsAllowedForUrls` (or `Google\Chrome`) by hand.

Placing a window on another screen takes the Window Management API, which browsers give only to a
secure origin: https, or http on localhost. Over `http://<ip>` a workspace's windows open on the
main window's screen. `"web_server_secure_origin": {"ip": ..., "port": ...}`, with the same shape
and defaults, lists the origin in the `OverrideSecurityRestrictionsOnInsecureOrigin` policy, which
is what the flag "Insecure origins treated as secure" sets. The flags page doesn't show policies,
so that flag stays Disabled and empty there. `edge://policy` lists it, and `isSecureContext` in the
dashboard's DevTools console reads `true`. The policy hands those APIs to plain http, which anyone
on the network path can rewrite, so set it only for an address on a network you trust.

When the browser runs on another Windows PC, run `scripts/slick-trader-web.bat` there, as the
user who trades. Double-click it and it asks for the address and port, or pass them:
`slick-trader-web.bat 192.168.1.20 3000 [edge|chrome]`. It lists `http://<ip>:<port>` in both
policies for Edge and Chrome, and adds a Desktop and Start menu shortcut that opens the dashboard
as an app (`--app=`). `slick-trader-web.bat remove 192.168.1.20 3000` takes the entries and
shortcuts out again.

Chrome and Edge show a thin read-only address strip on a popup, and no page can remove it.
Starting the main window as an app drops its own address bar and tabs:
`msedge --app=http://<host>:3000` or `chrome --app=http://<host>:3000`. Skip `--kiosk`, which locks
the main window to full screen.

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
js/components/          picker, strategy control, workspaces menu, icons
js/tools/               ladder, order (the Ticket), orders, fills, positions, pnl, log
js/slick-ladder.js      the ladder canvas (@slickquant/slick-ladder)
```

There's no build step. Pages load ES modules through an import map. To update a library, run
`npm install` and copy its dist file into `lib/` under a `.js` name. The server sends only `.js`
files as JavaScript, and browsers refuse to run a module served as anything else.

Every window opens its own WebSocket. A main window and the tool windows it opened talk over a
`BroadcastChannel`, and each message carries its dashboard's id. Tool windows send log entries and
the main window relays the log. For a workspace, the main window asks its tool windows where they
are and closes them. A Ticket takes prefill params from its URL, or over the channel when it's
already open.

## API

REST: `GET /api/health` (status and engine version), `/api/brokers`, `/api/strategies`,
`/api/positions` and `/api/pnl` (each also per `/:strategy_id`), `/api/orders`, `/api/fills` (the
last 1,000 fills, oldest first), `/api/ladder-config`, `/api/workspaces`
(`{"last":<name>,"workspaces":{<name>:{"windows":[...]}}}`). `PUT /api/ladder-config`.
`PUT /api/workspaces` (`{name, windows}`, which also makes it the last used),
`DELETE /api/workspaces` (`{name}`), `PUT /api/workspaces/last` (`{name}`). Names are trimmed.
`POST /api/orders`, `PUT`/`DELETE /api/orders/:strategy/:order_id`,
`DELETE /api/orders/cancel-all`, `POST /api/strategies/:id/control`, `/halt` and `/resume`,
`GET`/`PUT /api/strategies/:id/config` and `/risk`. See [CONTROLS_GUIDE.md](CONTROLS_GUIDE.md).

WebSocket, client to server:

- `{"type":"subscribe","channel":"instruments"}`: the whole list right away, then additions.
- `{"type":"subscribe","channel":"level2","instrument":"ZNZ6"}` and `unsubscribe`.
- `{"type":"ping"}`.

Server to client:

- `welcome`, `pong`.
- `instruments` / `instruments_added`: `{"instruments":{"ZNZ6":[<broker id>,<tick>,<min qty>]}}`.
  The tick and the min qty are 0 when the broker gives none.
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
| `ladder_config` | `{"default_qty":0.01}` | `default_qty`, and per instrument `default_qty` and `default_broker_id`. An order's qty starts at the instrument's `default_qty`, else its broker's min qty, else `default_qty` |
| `web_server_workspaces_file` | `state/workspaces.json` | saved workspaces, written on each change |
| `web_server_allow_popups` | off | `{"ip", "port"}`, Windows only. Lists the origin in this user's Edge and Chrome `PopupsAllowedForUrls` |
| `web_server_secure_origin` | off | `{"ip", "port"}`, the same for `OverrideSecurityRestrictionsOnInsecureOrigin` |
| broker `market_data.rest_url` (Coinbase) | `https://api.coinbase.com` | where products are listed |

There's no authentication. Keep the port on localhost, or put it behind a proxy that adds TLS and auth.
