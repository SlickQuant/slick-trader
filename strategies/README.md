# SlickTrader Strategy Plugins

This directory contains strategy plugins that can be **dynamically loaded at runtime** without
recompiling the trading engine.

## Plugin Architecture

Strategies are compiled as shared libraries (.dll on Windows, .so on Linux/Mac) and loaded by
`StrategyManager` via the `StrategyLoader` class. Note what this architecture does and doesn't
buy you:
- **Live parameter updates, not code hot-reload**: `PUT /api/strategies/:id/config` reaches
  `on_config()` on an already-loaded strategy (over the event bus, see
  [Respect the Worker Thread](#7-respect-the-worker-thread)) and persists the change to that
  strategy's `<name>.config.json` in the configs folder — the same file the loader reads at startup, so the
  change survives a restart. No restart is needed to apply it. (Editing that file by hand is the
  other way round: nothing watches it, so a hand edit only takes effect on the next start.)
  But `StrategyManager::load_strategies()` only runs once, at engine startup
  ([trading_engine.cpp](../src/core/trading_engine.cpp)); nothing re-scans the plugins folder or
  swaps in a rebuilt `.dll`/`.so` at runtime. To pick up a rebuilt plugin binary, restart the
  engine.
- **No crash isolation**: plugins run in-process — same address space, same `StrategyManager`
  worker thread as the engine. Dispatch calls (`strategy->on_trade()`, `on_quote()`, etc. in
  [strategy_manager.cpp](../src/core/strategy_manager.cpp)) are plain virtual calls with no
  `try`/`catch` or SEH wrapping. An unhandled exception or a crash (e.g. null deref) inside a
  strategy takes down the whole engine — write defensively.
- **Flexibility**: Load a specific plugin at a given path via `StrategyLoader::load()`
- **Distribution**: Distribute strategy plugins as standalone `.dll`/`.so` files, built
  independently of the engine

The one exception is `ManualStrategy` ([manual_strategy.hpp](../include/slick-trader/strategy/manual_strategy.hpp)):
it's a built-in core strategy, not a plugin, and is always registered at startup to own orders
placed manually through the Web UI.

## Building Strategy Plugins

All strategies in this directory are automatically built as shared libraries:

```bash
cmake --build build --config Debug
# Plugins output to: build/strategies/<Config>/  (e.g. build/strategies/Debug/ on Windows,
# where <Config> comes from the Visual Studio multi-config generator; build/strategies/ on
# single-config generators like Ninja/Makefiles)
```

### Windows Output
- `build/strategies/Debug/market_maker.dll`
- `build/strategies/Debug/order_stacker.dll`

### Linux/Mac Output
- `build/strategies/libmarket_maker.so`
- `build/strategies/liborder_stacker.so`

## Creating Your Own Plugin

### 1. Create Strategy Class

Inherit from `StrategyBase`:

```cpp
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_export.hpp>

class MyStrategy : public slick::trader::StrategyBase {
public:
    MyStrategy() : StrategyBase("MyStrategy", "1.0.0") {}

protected:
    void on_quote(const QuoteEvent& event) override {
        // Your trading logic here
    }

    void on_fill(const OrderFillEvent& event) override {
        // Handle fills
    }
};
```

Override `on_instrument_definition(const InstrumentDefinitionEvent&)` to learn an instrument's
tick size, contract multiplier and quantity limits when its broker publishes them. Position, PnL
and risk accounting apply the contract multiplier on their own, whether or not a strategy does.

### Market data subscriptions

`get_subscriptions()` declares what the strategy needs, one `SubscriptionRequest` per broker and
kind of data. Each kind is a separate request to the broker, so a strategy that wants the book
and the trades of an instrument asks for both:

| `SubscriptionType` | Callback | Notes |
| --- | --- | --- |
| `TRADE` | `on_trade` | |
| `QUOTE` | `on_quote` | Top of book |
| `LEVEL2`, `ORDER_BOOK` | `on_order_book`, `on_quote` | Price levels; `on_quote` on each change of the top |
| `MBO` | `on_order_book_l3`, `on_quote` | Every order of the book. Only brokers whose feed carries it; the others log and ignore the request |

### Market-by-order books and queue position

With an `MBO` subscription the strategy is handed the instrument's order-by-order book after
each exchange event has been applied to it - never half way through one:

```cpp
#include <slick-trader/market_data/queue_position.hpp>

std::vector<SubscriptionRequest> get_subscriptions() const override {
    return {SubscriptionRequest(broker_id_, SubscriptionType::MBO, {instrument_})};
}

void on_order_book_l3(std::string_view instrument, const slick::orderbook::OrderBookL3& book) override {
    if (instrument != instrument_) return;   // every strategy sees every book

    // One of this strategy's own working orders, by its client order id.
    if (const auto position = get_queue_position(book, my_client_order_id_)) {
        // position->orders_ahead, position->quantity_ahead: what trades before it at its price
        // position->level_quantity, position->level_orders, position->level_index (0 = best)
    }
}
```

`get_queue_position()` answers nothing until the order is acknowledged *and* shows on the feed -
the two arrive on different connections, in either order - and nothing again once it is filled or
cancelled. It works for brokers that name an order on the market data feed by the id its
acknowledgement carries. `queue_position(book, order_id)` is the same lookup for any order of the
book. Both cost one hop per order ahead, and the book belongs to the worker thread: read it inside
a callback, do not keep the reference.

An instrument subscribed `MBO` and nothing else still gets `on_quote` and PnL marks, taken from
the order-by-order book; with a `LEVEL2` or `ORDER_BOOK` subscription as well, those keep coming
from the price-level book.

### 2. Export the Plugin

Add the export macro at the end of your .cpp file:

```cpp
// Export the strategy plugin
SLICK_TRADER_EXPORT_STRATEGY(MyStrategy)
```

This macro creates the required C functions:
- `create_strategy()` - Creates strategy instance
- `destroy_strategy()` - Destroys strategy instance
- `get_strategy_name()` - Returns strategy name
- `get_strategy_version()` - Returns strategy version
- `set_logger_instance()` - Points the plugin's logger at the host process's logger instance
- `set_broker_status_registry()` - Points the plugin at the host's `BrokerStatusRegistry`

The two `set_*` entry points exist because a plugin links `slick-trader-core` **statically**, so
it owns private copies of that library's singletons — on Windows the host exports no symbols for
them to bind to. `StrategyLoader` calls both before `create_strategy()`. Without the registry
injection a plugin reads an all-false broker status and `submit_order()` rejects every order as
"Broker order connector disconnected", while orders placed from inside the host still succeed. If
you hand-roll the exports instead of using the macro, export both or the plugin will never place
an order.

### 3. Add to CMakeLists.txt

Add one line to [strategies/CMakeLists.txt](CMakeLists.txt). The
`slick_trader_add_strategy()` function defined at the top of that file handles the boilerplate —
`SHARED` library, include paths, linking `slick-trader-core`, the empty `PREFIX`, the output
directory, and MSVC's `/FS`:

```cmake
slick_trader_add_strategy(my_strategy)
```

Sources default to `<name>.cpp`. Override them, and link extra targets, with the optional
`SOURCES` and `LINK` arguments:

```cmake
slick_trader_add_strategy(my_strategy
    SOURCES my_strategy.cpp my_helper.cpp
    LINK    slick::stacker
)
```

The two shipped plugins are declared exactly this way:

```cmake
slick_trader_add_strategy(market_maker)
slick_trader_add_strategy(order_stacker LINK slick::stacker)
```

If your plugin needs a third-party library, declare it in
[strategies/CMakeLists.txt](CMakeLists.txt) alongside the target rather than in the root
`CMakeLists.txt` — that keeps it out of the dependency graph for anyone who builds with
`-DSLICK_TRADER_BUILD_STRATEGIES=OFF`. `slick-stacker` is fetched there for this reason.

### Building the plugins at all

The whole directory is behind a CMake option:

```cmake
option(SLICK_TRADER_BUILD_STRATEGIES "Build the example strategy plugins" ${PROJECT_IS_TOP_LEVEL})
```

It defaults **ON** when SlickTrader is the top-level project and **OFF** when it is consumed as a
subproject, on the reasoning that the example plugins are for developing against this repo rather
than for consuming it. Turn it off explicitly with `-DSLICK_TRADER_BUILD_STRATEGIES=OFF`; the
engine still builds and still loads any plugin `.dll`/`.so` it finds at runtime, since plugins are
`dlopen`ed and never linked.

### 4. Add a Strategy Config File

Each strategy is configured through a `<name>.config.json` file dropped into the configs folder
(see [Automatic Loading](#automatic-loading-recommended) below). The filename itself doesn't
matter as long as it ends in `.config.json` — the file is matched to a loaded plugin by its
`name` field, which must equal the string returned by your strategy's `name()` (i.e. the first
constructor argument to `StrategyBase`).

Real example, [config/market_maker.config.json](../config/market_maker.config.json):

```json
{
    "name": "MarketMaker",
    "enabled": true,
    "parameters": {
        "broker": "HYPERLIQUID",
        "instrument": "BTC",
        "order_size": 0.005,
        "max_inventory": 0.010001,
        "min_profit_usd": 0.5
    },
    "risk_limits": {
        "max_loss_per_strategy": -200.0,
        "max_order_value": 10000.0,
        "max_orders_per_second": 10,
        "max_position_size": 0.05
    }
}
```

Schema (parsed by `StrategyConfig::from_json`, see
[strategy_config.hpp](../include/slick-trader/core/strategy_config.hpp)):
- `name` — must match `IStrategy::name()`
- `enabled` — whether to run the strategy (default: `true` if no config file is found at all)
- `risk_limits` — `max_position_size`, `max_order_value`, `max_orders_per_second`,
  `max_loss_per_strategy`, plus optional per-instrument overrides under `instruments`
- `parameters` — free-form JSON strategy authors read via `get_parameter<T>(key, default)`

## Loading Plugins

### Automatic Loading (recommended)

This is how strategies are loaded in practice. On startup, `StrategyManager::load_strategies()`
(see [strategy_manager.cpp](../src/core/strategy_manager.cpp)) scans a plugins folder for every
`.dll`/`.so` file, loads them all, and matches each loaded strategy to a `*.config.json` file
found in a configs folder by name (see [Add a Strategy Config File](#4-add-a-strategy-config-file)
above). Both folders are set in the top-level `config.json` under `strategies`:

```json
"strategies": {
    "enabled": true,
    "strategy_plugins_folder": "build/strategies/Debug",
    "strategy_configs_folder": "config"
}
```

A plugin with no matching config file is still loaded and defaults to enabled with default risk
limits; a plugin whose config file has `"enabled": false` is loaded but skipped.

### Manual Loading (advanced)

You can also load a specific plugin directly from C++ using `StrategyLoader`:

```cpp
#include <slick-trader/core/strategy_loader.hpp>

// Create loader
StrategyLoader loader;

// Load plugin
#ifdef _WIN32
    auto strategy = loader.load("strategies/my_strategy.dll");
#else
    auto strategy = loader.load("strategies/libmy_strategy.so");
#endif

if (strategy) {
    // Add to trading engine
    engine.add_strategy(strategy);

    // Set risk limits
    RiskLimits limits;
    limits.max_position_size = to_qty_t(1.0);
    engine.get_risk_manager().set_limits(strategy->name(), limits);
} else {
    LOG_ERROR("Failed to load: {}", loader.get_last_error());
}
```

### Error Handling

The loader provides detailed error messages:

```cpp
if (!strategy) {
    LOG_ERROR("Plugin load failed: {}", loader.get_last_error());
    // Possible errors:
    // - File not found
    // - Missing required functions
    // - Symbol resolution errors
    // - create_strategy() returned nullptr
}
```

## Reference Example: MarketMaker

[market_maker.cpp](market_maker.cpp) is the current, buildable reference plugin — a
production-ready market maker with:
- Configurable fair value: `SIMPLE_MID`, `WEIGHTED_MID`, or `NLEVEL_WMID`
- Inventory skew: reservation price shifted against open position
- Trend detection: dual-EMA signal shifts quotes in trend direction
- Book imbalance: immediate microstructure pressure adjustment
- Quote refresh via direct order modify (single round-trip, no quote gap)
- A per-side quote-slot state machine (`EMPTY`/`PENDING_NEW`/`ACTIVE`/`PENDING_MODIFY`/`PENDING_CANCEL`)
- Post-only GTC orders only (maker-friendly)

Three things the slot machine has to get right, because each of the first two leaks live orders
if it does not:

- **A rejected amendment is not a dead order.** Venues reject an edit (Coinbase:
  `edit_failure_reason`) or a cancel while leaving the original resting. `PENDING_MODIFY` and
  `PENDING_CANCEL` + reject therefore return to `ACTIVE` and retry; every other reject clears the
  slot. Clearing it on an amendment failure leaves the original resting and puts a second order
  beside it.
- **Only an ack at the requested price closes a `PENDING_MODIFY`.** Venues keep streaming the
  order's pre-edit price while the edit is in flight. Accepting one of those as the modify ack
  drops the slot to `ACTIVE` at the stale price and fires a second, overlapping edit — which is
  what provokes the edit failure in the first place.
- **Every ack records where the order rests, even one that settles nothing.** An ack is the venue
  stating the order's current price and size, and that is what the order falls back to when an
  amendment is rejected. On a venue that accepts several edits in flight (CME) the fallback is the
  last edit it *accepted*, not the state the sequence started from.

### Prices at or below zero

Futures calendar spreads trade through zero, so the strategy never uses `0.0` as a "no price"
marker — `NaN` is, matching what `from_price_t(NULL_PRICE)` returns — and never divides by a
signed price. Two config parameters turn the support on:

| Parameter | Purpose |
|---|---|
| `allow_negative_price` | Accept zero/negative bids and asks as real prices instead of rejecting them as bad data. Default `false`, which keeps the historical behaviour for instruments that cannot trade there. |
| `tick_size` + `refresh_threshold_ticks` | Reprice on an absolute number of ticks instead of `refresh_threshold_bps`. Both must be set; together they override the bps threshold. |

The tick threshold is not optional in practice: a bps threshold is a fraction of the price, and
near zero that fraction collapses to nothing, so the strategy would reprice on every tick. Setting
`allow_negative_price` without it logs a warning. Tick mode also changes how the trend term is
normalised — by the half spread rather than by `|fair value| / 1000`, which is no scale at all for
an instrument quoted near zero.

All of its config parameters are documented in the file's header comment, and
[config/market_maker.config.json](../config/market_maker.config.json) shows a real config for it.

## Reference Example: OrderStacker

[order_stacker.cpp](order_stacker.cpp) is the second reference plugin, and the one to read if you
want to drive a **third-party order-management library** from `StrategyBase`. It maintains a
two-sided ladder ("stack") of resting post-only orders using the header-only
[slick-stacker](https://github.com/SlickQuant/slick-stacker) library: you hand the stacker one
price and one quantity per side, and it lays down the configured rungs behind that and keeps the
venue in agreement as acknowledgements, fills, cancels and rejects arrive.

With the shipped defaults (`offset_ticks: 0`, `levels: 4`, `level_gap_ticks: 1`, `tick_size: 0.01`):

```
  ask ladder                      bid ladder
  100.04                          100.00   <- best_bid, level 0
  100.03                           99.99
  100.02                           99.98
  100.01                           99.97
  100.00  <- best_ask, level 0     99.96
```

### How the integration works

- **The executor bridge.** slick-stacker's `OrderExecutor` concept needs an *integral* order id,
  but slick-trader identifies orders by `std::string client_order_id` plus a broker-assigned
  `order_id` that only arrives with the ack. A private nested `Executor` class holds a
  back-pointer to the strategy — being a nested class, it reaches `StrategyBase`'s protected
  `submit_order()`/`cancel_order()`/`modify_order()` directly, with no `friend` declaration and no
  `std::function` indirection. It allocates monotonic `uint64_t` ids of the form `(seq << 1) | side`
  so the side survives the record being erased.
- **`ack_required = true` is a prerequisite, not a tuning knob.** `cancel_order()` needs the broker
  `order_id`, which does not exist until the ack. `ack_required=true` is what stops the stacker
  asking for a cancel or modify any earlier.
- **Prices are snapped to the tick grid**, rounding away from the market on both sides. An off-grid
  price makes the stacker's `depth_of()` return an invalid depth, which it answers with a full
  rebase — cancelling every order, every tick. Rounding away from the market also means rounding
  can never push a price across the spread.
- **`offset_ticks = 0` quotes at the touch.** When `tick_size` equals the venue tick, the snap is
  an identity and level 0 sits exactly on the best bid/ask. Set `tick_size` coarser and the quote
  snaps to the grid instead, sitting up to one tick behind — safe, but not what the name implies.
  Expect a higher post-only reject rate at offset 0; the stacker re-places those on the next pass.
- **One `reconcile()` per event.** The stacker's `on_*` handlers never send — all I/O happens in
  `reconcile()`. The library wants one reconcile per event *batch*, but the engine dispatches one
  event at a time, so the plugin reconciles at the end of every handler behind a re-entrancy guard.
- **A `max_position` guard** pulls the offending side once the filled position reaches the limit,
  with a hysteresis band so it cannot flap. Note it sees only the *filled* position, so peak
  exposure can overshoot by up to one ladder before the pull lands.

Every config parameter is documented in the file's header comment;
[order_stacker.json](order_stacker.json) is the template to copy to
`config/order_stacker.config.json`.

## Plugin Best Practices

### 1. Use StrategyBase
Inherit from `StrategyBase` to get:
- Automatic position tracking on fills
- Risk validation before order submission
- Order submission helpers
- Access to position tracker, PnL calculator, risk manager

### 2. Keep client_order_id ownership intact

`StrategyManager` broadcasts every ack/fill/reject/cancel to every loaded strategy, so
`StrategyBase` decides which events are yours from the `<strategy-name>_` prefix that
`generate_order_id()` stamps on `client_order_id`; everything else is dropped before it can reach
your `on_ack`/`on_fill`/`on_reject`/`on_cancel` hooks or the position tracker. Two consequences:

- If you override `generate_order_id()` with a different id scheme, override
  `owns_client_order_id()` to match — otherwise your own fills are ignored and your position
  never updates.
- Keep strategy names distinct enough that no name is a `<name>_` prefix of another
  (`MM` and `MM_Fast` collide; `MarketMaker` and `OrderStacker` do not).

### 3. Set Risk Limits
Always configure risk limits for your strategy:
```cpp
RiskLimits limits;
limits.max_position_size = to_qty_t(10.0);      // Max position
limits.max_order_value = to_price_t(100000.0);  // Max order value
limits.max_orders_per_second = 50;              // Rate limit
engine.get_risk_manager().set_limits(strategy->name(), limits);
```

### 4. Handle Errors Gracefully
Check for null pointers:
```cpp
if (!get_position_tracker()) {
    LOG_WARN("Position tracker not available");
    return;
}
```

### 5. Log Important Events
Use the logging framework:
```cpp
LOG_INFO("MyStrategy: Submitted order {}", order_id);
LOG_WARN("MyStrategy: Position limit reached");
LOG_ERROR("MyStrategy: Invalid state");
```

### 6. Clean Up Resources
Implement proper cleanup in destructor:
```cpp
~MyStrategy() override {
    // Cancel pending orders
    // Close positions
    // Release resources
}
```

### 7. Respect the Worker Thread
Market-data and order callbacks all run on the single `StrategyManager` worker thread, so state
touched only from those handlers needs no locking. **`on_config()` runs there too**, which means
you can reconfigure worker-owned state directly inside it — no latch, no lock:

```cpp
void on_config(const nlohmann::json& params) override {
    // Worker thread. Touch whatever your event handlers touch.
}
```

That is what routing config updates over the event bus buys you. `PUT /api/strategies/:id/config`
does **not** call `set_config()` from the uWS thread; it publishes a `StrategyConfigEvent` onto
the same `stream_buffer_multiplexer` the market data rides
([strategy_config_event.hpp](../include/slick-trader/events/strategy_config_event.hpp)), and
`StrategyManager::dispatch_strategy_config()` parses it and calls `set_config()` from the worker
loop. The parameters travel as JSON text in a trailing flexible array, the same idiom
`Level2UpdateEvent` uses for its price levels and `MboUpdateEvent` for its orders — publish with
`size_for(text.size())`, never `sizeof(StrategyConfigEvent)`.

Two things still are not on the worker thread:

- `on_stop()` runs on the caller's thread, *before* the worker thread is joined
- anything your own strategy spawns

A config update arrives whether or not the strategy is running, and applying one is not the same
as trading on it — so if your `on_config()` acts on the change immediately, give a stopped
strategy the same treatment your market-data handler does. `order_stacker.cpp` pulls its ladder
before reconciling, so an update to a paused strategy cannot put orders back on the book.

## Platform-Specific Notes

### Windows
- Plugins use `.dll` extension
- No `lib` prefix
- Use `/FS` compiler flag for parallel builds
- Visual Studio: Set output directory to avoid PDB conflicts

### Linux/Mac
- Plugins use `.so` extension
- Include `lib` prefix
- Use `-fPIC` for position-independent code
- Set `RPATH` for dependency resolution

## Troubleshooting

### "Failed to load library"
- Check file path is correct
- Verify plugin was built successfully
- On Linux: Check `ldd` for missing dependencies
- On Windows: Check dependencies with `dumpbin /dependents`

### "Library missing required functions"
- Make sure `SLICK_TRADER_EXPORT_STRATEGY(ClassName)` is present
- Check for typos in class name
- Rebuild plugin completely

### "create_strategy() returned nullptr"
- Check strategy constructor doesn't throw
- Verify all dependencies are available
- Add logging to constructor to debug

### "No config find for strategy X" / strategy running with unexpected defaults
- Your config file's `name` field doesn't match the strategy's `name()` string
- Confirm the file lives in `strategy_configs_folder` and ends in `.config.json`

## Additional Resources

- [StrategyBase API](../include/slick-trader/strategy/strategy_base.hpp)
- [IStrategy Interface](../include/slick-trader/strategy/istrategy.hpp)
- [StrategyLoader](../include/slick-trader/core/strategy_loader.hpp)
- [StrategyManager (auto-discovery logic)](../include/slick-trader/core/strategy_manager.hpp)
- [StrategyConfig (config file schema)](../include/slick-trader/core/strategy_config.hpp)
- [ManualStrategy](../include/slick-trader/strategy/manual_strategy.hpp)
