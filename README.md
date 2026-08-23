# SlickTrader

A multi-broker algorithmic trading engine written in modern C++23. SlickTrader connects to
crypto exchanges over WebSocket/REST, routes market data and order events through a lock-free
messaging core, runs trading strategies as hot-loadable plugins, and exposes a real-time web
dashboard for monitoring and manual control.

> **Status**: active development. The Coinbase and Hyperliquid brokers are wired up; order
> execution against live exchanges requires broker credentials to be configured (see
> [Broker Configuration](#broker-configuration) below) and is disabled by default in the sample
> configs.

## Features

- **Multi-broker support** — [Coinbase Advanced Trade](https://github.com/SlickQuant/coinbase-advanced-cpp)
  and [Hyperliquid](https://github.com/SlickQuant/hyperliquid-cpp) today, with market data and
  order execution modeled as independent, per-broker connectors so more venues can be added
  behind the same [`IBroker`](include/slick-trader/brokers/ibroker.hpp) interface.
- **Lock-free messaging core** — a single-writer threading model (see
  [Architecture](#architecture)) built on [slick-queue](https://github.com/SlickQuant/slick-queue)
  and a `stream_buffer_multiplexer`, avoiding mutexes on the hot path between broker connectors,
  the strategy manager, and the web server.
- **Hot-loadable strategy plugins** — strategies are compiled as standalone `.dll`/`.so` shared
  libraries and loaded at engine startup by
  [`StrategyLoader`](include/slick-trader/core/strategy_loader.hpp); no engine rebuild needed to
  iterate on strategy logic. See [strategies/README.md](strategies/README.md) for the plugin
  guide, config schema, and a production-style `MarketMaker` reference implementation.
- **Position, PnL & risk management** — FIFO-lot position tracking, realized/unrealized PnL, and
  pre-trade risk validation (position limits, order-value limits, per-second rate limits,
  max-loss halts) all run single-threaded off the strategy manager for lock-free reads/writes.
- **Live web dashboard** — a dependency-free vanilla-JS frontend (`web/static/`) served over
  uWebSockets, with a REST API and a price-ladder UI for manual order entry/cancellation.
- **Manual + automated strategies side by side** — a built-in `ManualStrategy` owns orders placed
  by hand through the dashboard, coexisting with plugin strategies under the same risk/position
  tracking.

## Architecture

```
                           ┌────────────────────────┐
                           │      TradingEngine     │  (owns lifecycle, wiring)
                           └────────────┬───────────┘
                                        │
         ┌──────────────────────────────┼──────────────────────────────┐
         │                              │                              │
         ▼                              ▼                              ▼
┌──────────────────┐           ┌──────────────────┐           ┌──────────────────┐
│  Broker threads  │           │  StrategyManager │           │    WebServer     │
│  (per-broker MD  │──events──▶│  (single-writer: │──events──▶│  (uWebSockets,   │
│  + order conns,  │◀──orders──│  positions, PnL, │◀───ctrl───│  REST + WS push, │
│   own threads)   │           │  risk, dispatch) │           │  manual control) │
└──────────────────┘           └──────────────────┘           └──────────────────┘
```

- Each broker owns a market-data connector and an order connector, each running on its own
  thread, publishing/consuming events through per-producer lock-free queues multiplexed by a
  `slick::stream_buffer_multiplexer`.
- `StrategyManager` is the **sole writer** to `PositionTracker`, `PnLCalculator`, and
  `RiskManager` — by construction there's a single thread mutating shared trading state, so no
  locking is required there. It dispatches market data/order events to every loaded strategy and
  forwards new/modify/cancel order requests to the owning broker's order connector.
  `StrategyBase` (which real strategies inherit from) validates every order against the
  `RiskManager` and checks `BrokerStatusRegistry` for connector health before it's queued.
  Order events are broadcast to all strategies, so `StrategyBase` attributes each one by the
  `<strategy-name>_` prefix `generate_order_id()` stamps on `client_order_id` and ignores the
  rest — without that, one strategy books another's fills into its own position and PnL.
- `WebServer` reads the same tracker/calculator/risk state and streams updates to connected
  browsers over WebSocket, and exposes a REST API for querying and controlling strategies/orders.
- `BrokerStatusRegistry` is a lock-free, always-on status board (market-data/order
  enabled+connected flags per broker) that both the dashboard and `StrategyBase::submit_order`
  consult to avoid orders getting stuck when a broker connector is down. Strategy plugins link
  `slick-trader-core` statically and so would otherwise get their own private copy of it, so
  `StrategyLoader` injects the host's instance into every plugin at load time (alongside the
  logger) — see [strategies/README.md](strategies/README.md).

## Requirements

- **Compiler**: MSVC (Visual Studio 2022) with C++23 support. The project targets Windows
  primarily; `CMakeLists.txt` also has Unix branches for `dlopen`-based plugin loading.
- **CMake** ≥ 3.28
- **[vcpkg](https://github.com/microsoft/vcpkg)** — provides `uWebSockets` and `nlohmann_json`
  (both `find_package(... REQUIRED)`, not auto-fetched). Configure with vcpkg's toolchain file.
- Everything else (`slick-logger`, `slick-orderbook`, `slick-net`, `coinbase-advanced-cpp`,
  `hyperliquid-cpp`) is pulled automatically via CMake `FetchContent` if not already installed.
  `slick-stacker` is fetched too, but only when the strategy plugins are being built — it is
  needed solely by `order_stacker`.
- Optional: a system **Lua 5.4** or **Python3** (dev headers) if you want to enable
  `SLICK_TRADER_ENABLE_LUA_STRATEGY` / script-based strategies (Python auto-detection is on by
  default; Lua is off by default).

## Building

Configure (first time, or after `CMakeLists.txt` changes):

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" \
  -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=Y:/repo/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Build:

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" \
  --build build --config Debug --target ALL_BUILD -j 16
```

This produces:
- `build/Debug/slick-trader.exe` — the main engine executable
- `build/strategies/Debug/*.dll` — strategy plugins (e.g. `market_maker.dll`, `order_stacker.dll`)
- `build/tests/Debug/slick-trader-tests.exe` — the regression test suite

### Tests

```bash
ctest --test-dir build -C Debug --output-on-failure
# or run the binary directly:
build/tests/Debug/slick-trader-tests.exe
```

The suite covers behaviour that only reproduces across a module boundary: that a plugin loaded
through `StrategyLoader` observes the host's `BrokerStatusRegistry`, and that a strategy ignores
order events belonging to other strategies. Requires GTest (`find_package(GTest CONFIG)`).

### Build Options

| Option | Default | Effect |
|---|---|---|
| `SLICK_TRADER_BUILD_STRATEGIES` | ON when top-level, OFF as a subproject | Build the example strategy plugins. When OFF, `slick-stacker` is not fetched either — it is needed only by `order_stacker`. The engine is unaffected: plugins are `dlopen`ed at runtime, never linked. |
| `SLICK_TRADER_DISABLE_CXX_MODULE_SCAN` | ON | Skip C++ module dependency scanning for faster MSVC builds |
| `SLICK_TRADER_FAST_FETCHCONTENT` | ON | Skip FetchContent update checks on every configure |
| `SLICK_TRADER_SUPPRESS_VS_REGEN` | ON | Disable `ZERO_CHECK` auto-regeneration for Visual Studio generators |
| `SLICK_TRADER_ENABLE_PYTHON_STRATEGY` | ON | Auto-detect Python3 for script-based strategies |
| `SLICK_TRADER_ENABLE_LUA_STRATEGY` | OFF | Auto-detect Lua 5.4 for script-based strategies |
| `SLICK_TRADER_BUILD_TESTS` | ON when top-level, OFF as a subproject | Build `slick-trader-tests` and its probe plugin. Requires GTest. |

See [strategies/README.md](strategies/README.md) for how to write and build your own strategy
plugin.

## Running

```bash
build/Debug/slick-trader.exe config/config_example.json
```

The engine reads a single JSON config path as its only argument. Copy
[`config/config_example.json`](config/config_example.json) to `config/config.json` (gitignored —
only the example is checked in) and adjust it for your environment; on startup it will:

1. Initialize per-broker market data and order connectors as enabled in the config.
2. Scan `strategies.strategy_plugins_folder` for plugins and load each one, matching it to a
   `*.config.json` in `strategies.strategy_configs_folder` by strategy name (see
   [strategies/README.md](strategies/README.md#4-add-a-strategy-config-file)).
3. Start the web dashboard on `web_service.web_server_port` if `web_service.enabled` is true.

Once running, open `http://localhost:<web_server_port>` (default `3000`) for the dashboard.
Shut down gracefully with Ctrl+C (SIGINT/SIGTERM).

### Configuration reference

Top-level keys in `config.json` ([example](config/config_example.json)):

| Key | Purpose |
|---|---|
| `log_file_path`, `log_level` | Log output path and verbosity (`trace`/`debug`/`info`/...) |
| `event_queue_size`, `order_request_queue_size`, `md_request_queue_size` | Lock-free queue capacities (entries) |
| `brokers[]` | Per-broker `{name, enabled, market_data:{...}, order:{...}}` — see below |
| `strategies` | `enabled`, `strategy_plugins_folder`, `strategy_configs_folder`, plus TOB (top-of-book) queue sizing |
| `web_service` | `enabled`, `web_server_port`, `web_server_cors`, `web_server_static_path`, `ladder_config` (default order size, per-instrument overrides), `config_queue_size`/`config_control_size` (event-bus producer the dashboard publishes strategy parameter updates through) |

### Broker configuration

- **Coinbase** — `market_data.websocket_url` (default `wss://advanced-trade-ws.coinbase.com`) and
  `order.rest_url` / `order.websocket_url` for the authenticated order channel. Order submission
  requires `COINBASE_API_KEY` / `COINBASE_API_SECRET` environment variables to be set.
- **Hyperliquid** — `market_data.api_url` / `order.api_url` (default
  `https://api.hyperliquid.xyz`). Order execution needs a wallet private key wired up to the
  order connector (see [`hyperliquid_order_connector.cpp`](src/brokers/hyperliquid/hyperliquid_order_connector.cpp));
  it is disabled (`order.enabled: false`) in both sample configs until that's configured.

Both brokers' local dev config (`config/config.json`, gitignored) point at `localhost` mock
endpoints rather than the real exchanges — check that file if you're running against simulators.

## Web Dashboard

The dashboard (`web/static/`) is a build-tool-free HTML/CSS/vanilla-JS app that connects over
WebSocket for live position/PnL/order updates and hits a REST API for everything else. Key
endpoints (see [`web_server.cpp`](src/web/web_server.cpp)):

- `GET /api/health`, `GET /api/brokers`, `GET /api/strategies`
- `GET /api/positions[/:strategy_id]`, `GET /api/pnl[/:strategy_id]`
- `GET /api/orders`, `POST /api/orders`, `PUT|DELETE /api/orders/:strategy_id/:order_id`, `DELETE /api/orders/cancel-all`
- `POST /api/strategies/:strategy_id/{control,halt,resume}`
- `GET|PUT /api/strategies/:strategy_id/{risk,config}`
- `GET|PUT /api/ladder-config`

It includes a price-ladder view (`slick-ladder`, an `@slickquant` npm package — see
[package.json](package.json)) for click-to-trade manual order entry; manual orders are attributed
to the built-in `ManualStrategy`. See [web/static/CONTROLS_GUIDE.md](web/static/CONTROLS_GUIDE.md)
and [web/static/FEATURES.md](web/static/FEATURES.md) for UI details.

## Project Layout

```
include/slick-trader/   Public headers (.hpp), organized by domain:
  core/                  TradingEngine, StrategyManager, PositionTracker, PnLCalculator,
                         RiskManager, BrokerFactory, BrokerStatusRegistry, StrategyLoader
  brokers/               IBroker + per-venue implementations (coinbase/, hyperliquid/)
  strategy/              IStrategy, StrategyBase, ManualStrategy, plugin export macro
  events/                Wire event types (trades, quotes, order book, order lifecycle)
  market_data/           Order book types
  web/                   WebServer
  utils/                 Fixed-point price/qty types, time helpers, fee structures
src/                     Implementation (.cpp) mirroring include/, plus main.cpp
strategies/              Strategy plugins, built as shared libraries (see its README)
tests/                   GTest regression suite, plus a probe plugin for cross-module tests
web/static/              Dashboard frontend (HTML/CSS/JS)
config/                  Runtime JSON configs (only config_example.json is checked in)
```

## Strategy Development

Strategies inherit from [`StrategyBase`](include/slick-trader/strategy/strategy_base.hpp), which
provides order submission/cancellation/modification helpers with built-in pre-trade risk checks
and broker-connectivity checks, automatic FIFO position tracking on fills, and order-state
tracking (`get_order`, `get_open_orders`, `close_all_positions`, ...). The full plugin guide —
project setup, `CMakeLists.txt` wiring, config schema, loading, and troubleshooting — lives in
[strategies/README.md](strategies/README.md). [`strategies/market_maker.cpp`](strategies/market_maker.cpp)
is a production-style reference implementation (configurable fair value, inventory skew, trend
detection, book-imbalance adjustment, post-only quoting via a per-side state machine).
[`strategies/order_stacker.cpp`](strategies/order_stacker.cpp) is the reference for integrating a
third-party order-management library: it maintains a two-sided ladder of resting orders offset a
configurable number of ticks from the top of book, driving `slick-stacker` through an executor
adapter over `StrategyBase`.

## Dependencies

| Library | Role |
|---|---|
| [slick-logger](https://github.com/SlickQuant/slick-logger) | Logging |
| [slick-orderbook](https://github.com/SlickQuant/slick-orderbook) | L2 order book maintenance |
| [slick-stacker](https://github.com/SlickQuant/slick-stacker) | Header-only resting-order ladder engine (used by the `order_stacker` strategy plugin) |
| [slick-net](https://github.com/SlickQuant/slick-net) | Networking (fetched transitively by the broker SDKs) |
| [slick-queue](https://github.com/SlickQuant/slick-queue) / `stream_buffer_multiplexer` | Lock-free single-producer event queues, multiplexed for broadcast to strategies/web |
| [coinbase-advanced-cpp](https://github.com/SlickQuant/coinbase-advanced-cpp) | Coinbase Advanced Trade API client |
| [hyperliquid-cpp](https://github.com/SlickQuant/hyperliquid-cpp) | Hyperliquid API client |
| [uWebSockets](https://github.com/uNetworking/uWebSockets) (via vcpkg) | REST + WebSocket server |
| [nlohmann/json](https://github.com/nlohmann/json) (via vcpkg) | JSON parsing/serialization |

## Conventions

- Headers use the `.hpp` extension throughout.
- New non-trivial state is single-writer/lock-free by design rather than mutex-protected — see
  [Architecture](#architecture).
- Bug fixes should come with a regression test.

## License

MIT — see [LICENSE](LICENSE).
