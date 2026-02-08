# SlickTrader Examples

This directory contains example applications demonstrating the SlickTrader framework.

## Example Trading System

A complete trading system example with:
- Coinbase integration (market data and order execution)
- Two example strategies:
  - **SimpleLoggerStrategy**: Logs all market data and order events
  - **SimpleMarketMaker**: Places bid/ask orders around mid-price (commented out by default)

### Prerequisites

1. **Build SlickTrader**:
   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   ```

2. **Coinbase API Credentials** (optional, only needed for order submission):
   - Create an account at [Coinbase Advanced Trade](https://www.coinbase.com/advanced-trade)
   - Generate API keys with trading permissions
   - Set environment variables:
     ```bash
     export COINBASE_API_KEY="your_api_key"
     export COINBASE_API_SECRET="your_api_secret"
     ```

### Running the Example

#### Market Data Only (No API Keys Required)

```bash
cd build/examples
./example_trading_system config.json
```

This will:
- Connect to Coinbase WebSocket for market data
- Subscribe to BTC-USD and ETH-USD trades and quotes
- Log market data events via SimpleLoggerStrategy
- Run until you press Ctrl+C

#### With Market Making (Requires API Keys)

1. Edit `example_trading_system.cpp` and uncomment the market maker section:
   ```cpp
   auto market_maker = std::make_shared<SimpleMarketMaker>(
       "BTC-USD",  // instrument
       20.0,       // spread in basis points (0.2%)
       0.001       // order size in BTC
   );
   engine.add_strategy(market_maker);
   ```

2. Rebuild:
   ```bash
   cmake --build . --config Release
   ```

3. Run with API credentials:
   ```bash
   cd build/examples
   export COINBASE_API_KEY="your_api_key"
   export COINBASE_API_SECRET="your_api_secret"
   ./example_trading_system config.json
   ```

This will:
- Connect to Coinbase for both market data and order execution
- Subscribe to market data
- Run SimpleLoggerStrategy (logs events)
- Run SimpleMarketMaker (places bid/ask orders)
- Display all order acks, fills, and rejects

### Configuration

Edit `config.json` to adjust queue sizes:

```json
{
  "md_queue_size": 16777216,          // Market data queue (16 MB)
  "order_request_queue_size": 1048576, // Order request queue (1 MB)
  "order_reply_queue_size": 1048576    // Order event queue (1 MB)
}
```

### Example Strategies

#### SimpleLoggerStrategy

Located in `strategies/simple_logger_strategy.h`.

Features:
- Logs every 100th trade and quote
- Logs all order events (acks, fills, rejects, cancels)
- Displays event counters on shutdown
- Good for monitoring and debugging

#### SimpleMarketMaker

Located in `strategies/simple_market_maker.h`.

Features:
- Places limit orders on both sides of the market
- Configurable spread (in basis points)
- Configurable order size
- Cancels orders on shutdown
- Demonstrates full order lifecycle

Parameters:
- `instrument`: Trading pair (e.g., "BTC-USD")
- `spread_bps`: Spread in basis points (e.g., 20 = 0.2%)
- `order_size`: Order size in base currency

### Creating Your Own Strategy

1. Create a new header file in `strategies/`:
   ```cpp
   #pragma once
   #include <slick-trader/strategy/strategy_base.h>

   class MyStrategy : public slick::trader::StrategyBase {
   public:
       MyStrategy() : StrategyBase("MyStrategy", "1.0.0") {}

   protected:
       void on_trade(const slick::trader::TradeEvent& event) override {
           // Your logic here
       }

       void on_quote(const slick::trader::QuoteEvent& event) override {
           // Your logic here
       }

       void on_order_fill(const slick::trader::OrderFillEvent& event) override {
           // Your logic here
       }
   };
   ```

2. Include and register in `example_trading_system.cpp`:
   ```cpp
   #include "../strategies/my_strategy.h"

   auto my_strategy = std::make_shared<MyStrategy>();
   engine.add_strategy(my_strategy);
   ```

3. Rebuild and run!

### Event Flow

```
Coinbase WebSocket
       ↓
   MD Connector (thread)
       ↓
   MD Queue (lock-free)
       ↓
 Strategy Manager (thread)
       ↓
   Strategies (SimpleLogger, SimpleMarketMaker, ...)
       ↓
 Order Queue (lock-free)
       ↓
 Order Connector (thread)
       ↓
   Coinbase REST API
       ↓
   Order Events ← WebSocket USER channel
       ↓
 Strategy Manager
       ↓
   Strategies (on_order_ack, on_order_fill, etc.)
```

### Safety Notes

⚠️ **WARNING**: The SimpleMarketMaker is for demonstration purposes only!

- Always test on testnet or sandbox first
- Start with very small order sizes
- Monitor your positions and P&L
- Understand the risks of market making
- The example does NOT include:
  - Position tracking
  - Risk management
  - P&L calculation
  - Error recovery
  - Order book management

### Next Steps

- Implement position tracking
- Add risk management (position limits, daily loss limits)
- Create more sophisticated strategies
- Add backtesting support
- Implement order book data structure
- Add performance metrics and monitoring
