# v0.3.0 - 11-08-2026

## Changed

- `ManualStrategy` adopts the instruments its config lists as brokers define them and subscribes
  their level 2 books, so the ladder works with no plugin loaded. The list is the `instruments`
  parameter of a `<name>.config.json` that names `MANUAL`: symbols, or prefixes ending in `*`
  (`"*"` for all). A list sent at runtime replaces it. The strategy manager keeps no book of these,
  so strategies get no book or quote for them, and a position in one is marked at its fills.
- `IStrategy::on_order_book()` takes the book's instrument. Every strategy is still handed the books
  any strategy subscribed, so a plugin should check the name. `IStrategy::initialize()` drops
  `md_request_queue`. `StrategyBase::submit_order()` takes an optional `refusal` string that gets
  the reason when it refuses an order.
- Plugins name the `STRATEGY_API_VERSION` they were built against (`SLICK_TRADER_EXPORT_STRATEGY`).
  The loader refuses any other version, including every plugin built before this change, so
  rebuild them.
- The strategy manager keeps no L2 book of an instrument no strategy subscribed. Past 65,536 book
  ids it refuses one rather than let two share a book.
- Each broker has its own market data request queue (`MdRequestQueues`), in shared memory
  `<md_request_queue_shm_name>_<BROKER>` if named, and its factory gets that one. A configured
  broker whose name matches no `BrokerId` fails startup.
- A market data connector reports how far it read its queue with
  `BrokerStatusRegistry::set_md_request_cursor()`, as `process_md_request()` does for it. Its
  requests wait until it does. `process_md_request()` now logs a request it cannot read before
  dropping it.
- The ladder's instruments, each with its broker, come over the WebSocket: all of them as
  `instruments` on connect, then MANUAL's new ones as `instruments_added`, at most every 250 ms.
  `/api/strategies` lists no instruments and gives MANUAL's type as `builtin`.
- The web server reads the event bus for up to 1 ms a pass, not 100 events, and writes level 2
  messages with no JSON tree.
- Building needs slick-logger 2.4.0 and hyperliquid-cpp 0.4.2 or later.

## Fixed

- The Submit Order dialog listed Coinbase, Alpaca and Simulated whatever was built, and "Simulated"
  sent to Hyperliquid. It now lists the brokers the engine registered. A ladder click sent every
  order to Coinbase. It now goes to the instrument's broker, or to the `default_broker_id` of its
  `per_instrument` entry. A `default_broker_id` at the top of `ladder_config` is ignored.
- API errors went out as 200 OK, and no status line had a reason phrase. An order for a broker the
  engine lacks, or one taking no orders now, is refused with the reason, and an order that risk
  limits turn down gets a 422.
- Browsers kept the dashboard's scripts for an hour, so a reload after a restart could run old ones.
- An API request body that arrived in more than one piece was parsed from its last piece alone, so
  a long one, such as a MANUAL list, was refused as invalid JSON or applied cut short.
- A plugin whose config file had no `parameters` was never initialized, and its first order
  crashed the engine.
- A dashboard that subscribed after a book's snapshot got only later changes. The web server keeps
  every streamed book and sends it whole on subscribe, empty while its broker's market data is
  down. A client watches at most 1,024 books.
- A dashboard too far behind lost the messages uWS dropped. uWS now buffers up to 16 MB per client.
  Past that the server closes the connection, and the dashboard reconnects to a fresh list and book.
- A subscription request bigger than the market data request queue threw, and a burst bigger than
  the queue overwrote requests their connector had not read. Requests are now split and held until
  their connector has room. A connector that reads nothing holds up only its own requests. It is
  logged after `md_request_stall_timeout_ms` (5000), and only `stop()` gives up on it.
- The dashboard built a hidden ladder for every instrument and streamed every book. It now builds
  only the selected instrument's ladder and streams that one book.
- A reconnected dashboard kept ladders for instruments the server no longer lists.
- The strategy manager's worker never yielded its core once it had handled an event.
- In `NLEVEL_WMID` mode, the market maker took its fair value from whichever book ticked last, for
  any instrument. It now uses only its own instrument's book.

# v0.2.0 - 11-02-2026

## Added

- **Broker building blocks** under `brokers/common`, for brokers kept out of tree: exact price
  scaling, a level book, a top of book shared with the order worker, post-only policy, rate limit
  and fees, a fill journal and a memory-mapped session store, an order-by-order book publisher,
  market data and order event writers, polled socket links and the kill switch loop. CMake gains
  `slick_trader_find_codec()` and `slick_trader_require_slick_socket()`.
- `BrokerId::ICE` (6).

## Fixed

- A connector routed a modify or cancel to whichever of its sessions knew the client order id,
  whatever broker the request was for. `dispatch_order_request()` checks the broker first.

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
  connector, each on its own thread, behind `IBroker`. Setting `SLICK_TRADER_ENABLE_COINBASE` or
  `SLICK_TRADER_ENABLE_HYPERLIQUID` to OFF leaves that broker and its SDK out of the build.
- **Brokers register themselves by name.** `SLICK_TRADER_REGISTER_BROKER("name", Type)` puts a
  broker in `BrokerRegistry`, and a `brokers[]` config entry names the one it wants. An entry naming
  no broker in the binary stops startup with the list of those it carries. A broker whose
  constructor throws is reported, not left to terminate the process.
- **Out-of-tree brokers.** `-DSLICK_TRADER_EXTERNAL_BROKERS=<dir>[;<dir>...]` builds brokers kept in
  their own repositories into the binary. Each calls `slick_trader_add_broker()` as an in-tree
  broker does, and its `TESTS` are compiled into `slick-trader-tests`. Configure prints each
  directory as it is added, then the brokers it registered and the version its own `project()`
  declares. A directory that registers none gets a warning.
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

- C++23, CMake 3.28 or later, on Windows (MSVC), Linux (GCC 14) and macOS (Apple clang).
  Dependencies are used as installed packages when found and fetched otherwise; uWebSockets and
  nlohmann_json come from vcpkg.
- `SLICK_TRADER_SANITIZER` builds with ASan and UBSan (`address,undefined`) or TSan (`thread`).
- `slick-trader-tests`, a GTest regression suite, run by CI on all three platforms and under ASan,
  UBSan and TSan on Linux. Each release
  attaches a ready-to-run package for each: the engine, the strategy plugins, the dashboard and
  the sample config.
