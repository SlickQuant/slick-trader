# SlickTrader Strategy Plugins

This directory contains strategy plugins that can be **dynamically loaded at runtime** without
recompiling the trading engine.

## Plugin Architecture

Strategies are compiled as shared libraries (.dll on Windows, .so on Linux/Mac) and loaded by
`StrategyManager` via the `StrategyLoader` class. Note what this architecture does and doesn't
buy you:
- **Live parameter updates, not code hot-reload**: `PUT /api/strategies/:id/config` calls
  `on_config()` on an already-loaded strategy and persists the change to `config.json` — no
  restart needed. But `StrategyManager::load_strategies()` only runs once, at engine startup
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

### Linux/Mac Output
- `build/strategies/libmarket_maker.so`

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

### 3. Add to CMakeLists.txt

Add your strategy to `strategies/CMakeLists.txt`, following the pattern used for `market_maker`:

```cmake
add_library(my_strategy SHARED
    my_strategy.cpp
)

if(SLICK_TRADER_DISABLE_CXX_MODULE_SCAN)
    set_target_properties(my_strategy PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(my_strategy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(my_strategy PRIVATE
    slick-trader-core
)

set_target_properties(my_strategy PROPERTIES
    PREFIX ""
    OUTPUT_NAME "my_strategy"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
)

# Synchronize PDB writes on Windows
if(MSVC)
    target_compile_options(my_strategy PRIVATE /FS)
endif()
```

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

All of its config parameters are documented in the file's header comment, and
[config/market_maker.config.json](../config/market_maker.config.json) shows a real config for it.

## Plugin Best Practices

### 1. Use StrategyBase
Inherit from `StrategyBase` to get:
- Automatic position tracking on fills
- Risk validation before order submission
- Order submission helpers
- Access to position tracker, PnL calculator, risk manager

### 2. Set Risk Limits
Always configure risk limits for your strategy:
```cpp
RiskLimits limits;
limits.max_position_size = to_qty_t(10.0);      // Max position
limits.max_order_value = to_price_t(100000.0);  // Max order value
limits.max_orders_per_second = 50;              // Rate limit
engine.get_risk_manager().set_limits(strategy->name(), limits);
```

### 3. Handle Errors Gracefully
Check for null pointers:
```cpp
if (!get_position_tracker()) {
    LOG_WARN("Position tracker not available");
    return;
}
```

### 4. Log Important Events
Use the logging framework:
```cpp
LOG_INFO("MyStrategy: Submitted order {}", order_id);
LOG_WARN("MyStrategy: Position limit reached");
LOG_ERROR("MyStrategy: Invalid state");
```

### 5. Clean Up Resources
Implement proper cleanup in destructor:
```cpp
~MyStrategy() override {
    // Cancel pending orders
    // Close positions
    // Release resources
}
```

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
