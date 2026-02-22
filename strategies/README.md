# SlickTrader Strategy Plugins

This directory contains example strategy plugins that can be **dynamically loaded at runtime** without recompiling the trading engine.

## Plugin Architecture

Strategies are compiled as shared libraries (.dll on Windows, .so on Linux/Mac) and loaded using the `StrategyLoader` class. This allows:
- **Hot-reloading**: Update strategies without restarting the engine
- **Isolation**: Strategy crashes don't crash the engine
- **Flexibility**: Load/unload strategies dynamically
- **Distribution**: Distribute strategy plugins separately

## Building Strategy Plugins

All strategies in this directory are automatically built as shared libraries:

```bash
cmake --build build --config Debug
# Plugins output to: build/strategies/
```

### Windows Output
- `build/strategies/simple_strategy_plugin.dll`

### Linux/Mac Output
- `build/strategies/libsimple_strategy_plugin.so`

## Creating Your Own Plugin

### 1. Create Strategy Class

Inherit from `StrategyBase`:

```cpp
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_loader.hpp>

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

### 3. Add to CMakeLists.txt

Add your strategy to `strategies/CMakeLists.txt`:

```cmake
add_library(my_strategy SHARED
    my_strategy.cpp
)

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
```

## Loading Plugins at Runtime

### From C++ Code

```cpp
#include <slick-trader/strategy/strategy_loader.hpp>

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

## Example Plugin: SimpleStrategyPlugin

`simple_strategy_plugin.cpp` demonstrates:
- ✅ Market data consumption (trades, quotes)
- ✅ Order submission with risk validation
- ✅ Automatic position tracking (via StrategyBase)
- ✅ PnL calculation and logging
- ✅ Proper plugin export

### To Enable in Example

Edit `examples/example_trading_system.cpp` and uncomment lines 119-142:

```cpp
// Uncomment this block:
LOG_INFO("Loading strategy plugin...");
StrategyLoader loader;

#ifdef _WIN32
    const std::string plugin_path = "strategies/simple_strategy_plugin.dll";
#else
    const std::string plugin_path = "strategies/libsimple_strategy_plugin.so";
#endif

auto simple_strategy = loader.load(plugin_path);
if (simple_strategy) {
    engine.add_strategy(simple_strategy);

    RiskLimits limits;
    limits.max_position_size = to_qty_t(0.1);
    limits.max_order_value = to_price_t(10000.0);
    limits.max_orders_per_second = 10;
    engine.get_risk_manager().set_limits(simple_strategy->name(), limits);
}
```

Then rebuild and run:

```bash
cmake --build build --config Debug
./build/examples/Debug/example_trading_system.exe config.json
```

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

## Additional Resources

- [StrategyBase API](../include/slick-trader/strategy/strategy_base.hpp)
- [IStrategy Interface](../include/slick-trader/strategy/istrategy.hpp)
- [Strategy Loader](../include/slick-trader/strategy/strategy_loader.hpp)
- [Example Trading System](../examples/example_trading_system.cpp)
