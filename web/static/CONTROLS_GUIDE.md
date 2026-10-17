# Strategy Controls Guide

## Overview

The SlickTrader dashboard now includes interactive controls for managing strategies and submitting orders directly from the web interface.

## Features

### 🎯 Order Submission

**Location**: Top-right header, "📝 New Order" button

**How to Use**:
1. Click the "📝 New Order" button in the header
2. Fill in the order form:
   - **Strategy ID**: The strategy that will submit the order
   - **Instrument**: Trading pair (e.g., BTC-USD, ETH-USD)
   - **Side**: Buy or Sell
   - **Type**: Market or Limit
   - **Quantity**: Amount to trade (e.g., 0.01 for BTC)
   - **Price**: (Limit orders only) Target price
3. Click "Submit Order"
4. Confirmation appears as a toast notification

**Keyboard Shortcuts**:
- `ESC`: Close the modal
- Click backdrop to close

### 🎮 Strategy Controls

**Location**: Strategy P&L table, "Actions" column

**Available Actions**:

1. **▶ Start** (Green button)
   - Action: `start`
   - Use: Resume or start a paused/stopped strategy
   - Effect: Strategy begins processing market data and may submit orders

2. **⏹ Stop** (Red button)
   - Action: `stop`
   - Use: Halt a running strategy
   - Effect: Strategy stops processing events and submitting orders

3. **📝 New Order** (Gray button)
   - Opens order modal pre-filled with the strategy ID
   - Quick way to submit orders for a specific strategy

### 💡 Quick Order Submission

**From Strategy Table**:
1. Find the strategy you want to trade with
2. Click the "📝" button in the Actions column
3. Order modal opens with Strategy ID pre-filled
4. Enter Instrument, Side, Quantity, and submit!

## API Endpoints

### Submit Order
```
POST /api/orders
Content-Type: application/json

{
  "strategy_id": "SimpleLoggerStrategy",
  "instrument": "BTC-USD",
  "side": "buy",
  "type": "market",
  "quantity": 0.01,
  "price": 50000.00  // optional, for limit orders only
}
```

**Response** (202 Accepted):
```json
{
  "success": true,
  "message": "Order submission not yet implemented",
  "order": {
    "strategy_id": "SimpleLoggerStrategy",
    "instrument": "BTC-USD",
    "side": "buy",
    "type": "market",
    "quantity": 0.01
  }
}
```

### Control Strategy
```
POST /api/strategies/{strategy_id}/control
Content-Type: application/json

{
  "action": "stop"
}
```

**Valid Actions**: `start`, `stop`, `pause`, `resume`

**Response**:
```json
{
  "success": true,
  "message": "Strategy control not yet implemented",
  "strategy_id": "SimpleLoggerStrategy",
  "action": "stop"
}
```

## UI Components

### Order Modal

**Fields**:
- **Strategy ID** (text input, required)
- **Instrument** (text input, required, e.g., BTC-USD)
- **Side** (dropdown: Buy/Sell, required)
- **Type** (dropdown: Market/Limit, required)
- **Quantity** (number input, required, min: 0.00000001)
- **Price** (number input, conditional: shown only for Limit orders)

**Validation**:
- All required fields must be filled
- Quantity must be > 0
- Limit orders must have price > 0
- Real-time validation with error messages

**Features**:
- Auto-focus on first field
- Tab navigation through form
- ESC to close
- Click backdrop to close
- Warning banner for live trading

### Action Buttons

**Button Styles**:
- **Start**: Green background (`btn-success`)
- **Stop**: Red background (`btn-danger`)
- **Order**: Standard gray (`btn`)

**Icons**:
- ▶ Start (play symbol)
- ⏹ Stop (stop symbol)
- 📝 Order (memo emoji)

**Tooltips**: Hover over buttons to see action description

## Browser Behavior

### Notifications

**Toast Notifications** appear in the top-right corner:
- **Success** (green): Order submitted, strategy control successful
- **Error** (red): Validation failed, API error
- **Info** (blue): General information
- **Warning** (orange): Caution messages

**Auto-dismiss**: Notifications disappear after 3 seconds

### Console Logging

All actions are logged to the browser console (F12):
```javascript
[Dashboard] New Order button clicked
[Dashboard] Order submitted
[Dashboard] Strategy control: SimpleLoggerStrategy stop
```

And to the System Log panel on the dashboard.

## Testing

### Manual Test Order
```javascript
// In browser console (F12)
modalManager.showOrderModal('TestStrategy', 'BTC-USD');
```

### Manual Strategy Control
```javascript
// In browser console (F12)
dashboard.controlStrategy('SimpleLoggerStrategy', 'stop');
```

### API Test (curl)
```bash
# Submit order
curl -X POST http://localhost:3000/api/orders \
  -H "Content-Type: application/json" \
  -d '{
    "strategy_id": "TestStrategy",
    "instrument": "BTC-USD",
    "side": "buy",
    "type": "market",
    "quantity": 0.01
  }'

# Control strategy
curl -X POST http://localhost:3000/api/strategies/TestStrategy/control \
  -H "Content-Type: application/json" \
  -d '{"action": "stop"}'
```

## Implementation Status

### ✅ Completed
- Order submission UI and validation
- Strategy control buttons in table
- Toast notification system
- API endpoints (backend ready)
- Modal dialog system
- Form validation
- Responsive design

### ⏳ Pending Integration
- **Order Execution**: Backend accepts orders but doesn't execute to broker yet
- **Strategy Lifecycle**: Backend receives control commands but doesn't change strategy state yet
- **Real-time Status**: Strategy status (running/stopped) not yet tracked

## Security Notes

⚠️ **Important**:
- No authentication - localhost only
- Orders go to live market (when integrated)
- Risk limits apply on backend
- Client-side validation only prevents UI errors
- Server-side validation is enforced

## Troubleshooting

### Modal Doesn't Appear
1. Check browser console (F12) for errors
2. Verify `modalManager` is defined: `typeof modalManager` → should be "object"
3. Hard refresh: Ctrl+Shift+R (Windows) or Cmd+Shift+R (Mac)
4. Try incognito mode to bypass cache

### Buttons Don't Work
1. Check if strategies are loaded: `window.dashboard.strategies`
2. Look for console errors
3. Verify the Strategy P&L table has data
4. Hard refresh the page

### Form Validation Errors
- **Missing fields**: All required fields must be filled
- **Invalid quantity**: Must be a positive number
- **Limit order price**: Required when Type is "Limit"

## Future Enhancements

- [ ] Strategy status indicators (running/stopped/paused)
- [ ] Pause/Resume buttons
- [ ] Order templates/presets
- [ ] Bulk order submission
- [ ] Strategy parameter editing
- [ ] Advanced order types (Stop-Loss, Take-Profit)
- [ ] Order cancellation
- [ ] Position close buttons

---

**Version**: Phase 5.1
**Last Updated**: 2026-02-27
