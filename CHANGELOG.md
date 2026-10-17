# v0.1.0 - 10-17-2026

First release.

## Added

### Engine

- **Lock-free messaging core.** Broker connectors, the strategy manager and the web server exchange
  events and requests through slick-queue rings multiplexed by a `stream_buffer_multiplexer`, with
  no mutex on the path between them.
- **Single-writer trading state.** `StrategyManager` is the only thread that writes
  `PositionTracker`, `PnLCalculator` and `RiskManager`. It dispatches market data and order events
  to every strategy and forwards each order request to its broker's order connector alone.
- `TradingEngine` reads one JSON config and owns the lifecycle of the brokers, the strategies and
  the web server. It shuts down on SIGINT/SIGTERM.

### Brokers

- **Coinbase Advanced Trade** (market data and order entry) and **Hyperliquid** (market data;
  order entry needs a wallet key wired in). Each broker is a market data connector and an order
  connector, each on its own thread, behind `IBroker`.
- **Brokers register themselves by name.** `SLICK_TRADER_REGISTER_BROKER("name", Type)` puts a
  broker in `BrokerRegistry`, and a `brokers[]` config entry names the one it wants. An entry naming
  no broker in the binary stops startup with the list of those it carries. A broker whose
  constructor throws is reported, not left to terminate the process.
- **Out-of-tree brokers.** `-DSLICK_TRADER_EXTERNAL_BROKERS=<dir>[;<dir>...]` builds brokers kept in
  their own repositories into the binary. Each calls `slick_trader_add_broker()` as an in-tree
  broker does, and its `TESTS` are compiled into `slick-trader-tests`. Configure prints each one,
  with the version its own `project()` declares.
- `BrokerStatusRegistry`: each broker's market data and order connectors' enabled and connected
  state, lock-free. `StrategyBase::submit_order` checks it before queueing an order, and the
  dashboard shows it.

### Strategies

- **Strategy plugins.** A strategy is a shared library (`.dll`/`.so`) that `StrategyLoader` loads at
  startup and matches to a `*.config.json` by name. The host injects its logger and
  `BrokerStatusRegistry` into every plugin.
- `StrategyBase`: order submission, modification and cancellation with pre-trade risk and broker
  connectivity checks; order-state tracking (`get_order`, `get_open_orders`,
  `close_all_positions`); and FIFO position tracking on fills. Order events are broadcast to
  every strategy, and each takes its own by the `<strategy-name>_` prefix on `client_order_id`.
  An order a broker restates after a reconnect or a restart is taken up by that prefix, and so is
  a fill for an order the strategy no longer tracks.
- Strategy parameters can be changed at runtime from the dashboard, through the event bus.
- Reference plugins: **MarketMaker** (fair value, inventory skew, trend detection, book-imbalance
  adjustment, post-only quoting) and **OrderStacker** (a two-sided ladder of resting orders, driven
  by slick-stacker).
- `ManualStrategy` owns the orders placed by hand from the dashboard.

### Market data

- Trades, quotes, level 2 updates and order books, as subscribed per instrument.
- **Market-by-order books and queue position.** A strategy subscribed `SubscriptionType::MBO` is
  handed the instrument's order-by-order book (`on_order_book_l3`), and `queue_position()` says how
  many orders, and how much quantity, stand ahead of one of its own orders.
- `InstrumentDefinitionEvent`: each instrument's tick size and contract multiplier, as its broker
  defines it.
- `InstrumentStatusEvent`: an instrument's trading state - pre-open, open, halted, closed - handed to
  strategies through `on_instrument_status`.

### Positions, PnL and risk

- FIFO-lot positions with realized and unrealized PnL, and fees from the fill or from the broker's
  fallback `FeeStructure` (`FeeType::PER_UNIT` for per-contract fees).
- **Futures accounting.** A per-instrument contract multiplier scales PnL and order-value risk
  checks. A `DAY` time in force, and zero and negative prices (calendar spreads), are supported.
- **Multi-leg fills.** A fill of an order for a multi-leg instrument comes as the order's own fill
  (`FillScope::ORDER`) followed by one fill per leg (`FillScope::LEG`): the order moves at the
  spread's price, and the positions booked are the legs'.
- Pre-trade risk: position limits, order-value limits, per-second rate limits, and loss limits that
  halt a strategy.
- **Positions across restarts.** With `position_store` configured, every position and its open lots
  is kept in a memory-mapped file: recording a fill is a copy into memory, a process killed
  mid-update leaves the position as it was, and a restart carries on with what was held.
- **Fill journal replay.** A broker that journals its fills (`OrderFillEvent::journal_seq`) replays
  them at startup. The position store keeps how far into each broker's journal the strategies had
  got, so a fill they had is dropped and one still on its way when the process died is booked
  once. A fill can carry its contract multiplier (`OrderFillEvent::contract_multiplier`), which
  values a replayed fill before the instrument's definition arrives.

### Web dashboard

- A vanilla-JS dashboard served by uWebSockets: positions, PnL, orders and broker status pushed
  over WebSocket, PnL at most four times a second as the top of book moves.
- REST API for health, brokers, strategies, positions, PnL and orders, strategy control (start,
  stop, halt, resume), risk limits and parameters, and the ladder configuration.
- A price ladder for click-to-trade manual orders.

### Build

- C++23, CMake 3.28 or later. Dependencies are used as installed packages when found and fetched
  otherwise; uWebSockets and nlohmann_json come from vcpkg.
- `slick-trader-tests`, a GTest regression suite.
