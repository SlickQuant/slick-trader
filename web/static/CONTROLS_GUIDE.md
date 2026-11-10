# Trading and Strategy Controls

Orders go to the live market. Every order from the dashboard is booked to MANUAL, and MANUAL's
risk limits apply before it is sent.

## From the dashboard

- **Ladder**: click a level. See [FEATURES.md](FEATURES.md#ladder).
- **Order** window: instrument, broker, side, market or limit, time in force, quantity, price.
- **Orders** window: Modify (new price and quantity), Cancel, or Cancel all after a confirm.
- **Main window, Strategies**: ▶ start, ■ stop, ⚙ parameters as JSON. Saving applies them on the
  strategy's thread and writes them back to its `.config.json`.

## API

```bash
# An order. broker_id is the id /api/brokers lists. time_in_force: 0 GTC, 1 IOC, 2 FOK, 3 GTD, 4 DAY.
curl -X POST http://localhost:3000/api/orders -H "Content-Type: application/json" \
  -d '{"instrument":"BTC-USD","broker_id":1,"side":"buy","type":"limit","price":50000,"quantity":0.01,"time_in_force":0}'

# Modify, then cancel, an open order (strategy and order id as /api/orders lists them).
curl -X PUT http://localhost:3000/api/orders/MANUAL/<order_id> -H "Content-Type: application/json" \
  -d '{"client_order_id":"<client_order_id>","new_price":50010,"new_quantity":0.02}'
curl -X DELETE http://localhost:3000/api/orders/MANUAL/<order_id> -d '{"client_order_id":"<client_order_id>"}'

# Start or stop a strategy.
curl -X POST http://localhost:3000/api/strategies/MM/control -H "Content-Type: application/json" -d '{"action":"stop"}'
```

`POST /api/orders` answers 202 once the order is sent. It answers 400 for a bad request or an unknown broker, 503
when the broker takes no orders now, and 422 when risk limits turn the order down. Each error comes
with the reason.
