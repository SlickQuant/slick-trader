# Dashboard Windows

## Main window

- **Toolbar**: opens each tool. The Log button shows a red count of errors logged while the Log
  window is closed.
- **Brokers**: market data and order connection of each broker, live.
- **P&L totals**: total, realized and unrealized across strategies.
- **Strategies**: status (with the halt reason), P&L, start, stop, and a JSON editor for parameters.
  A halt or pause shows within 5 s.

## Ladder

- A searchable instrument dropdown. Type to filter, then use the arrow keys and Enter. It shows the
  first 200 matches.
- A new ladder opens with nothing picked. A reloaded one comes back on its instrument.
- Uses the tick the broker defined. Without one it falls back to a small table, then 0.01.
- Click a level to trade the ladder qty there. The ladder config sets the qty, and until it loads
  the qty box is disabled and clicks send nothing. A price typed in the box overrides the level. A limit order left on IOC goes out GTC, and a
  market order left on GTC goes out IOC.
- Your resting orders are outlined in gold, kept through a reload and moved on a modify.
- Several ladders can run side by side, each streaming one book.

## Order

One order for any listed instrument, booked to MANUAL. The broker defaults to the one listing the
instrument. A `per_instrument` `default_broker_id` in `ladder_config` overrides it.

## Orders

Open orders across strategies, with modify, cancel and cancel all. A burst of updates reloads the
list once.

## Fills

The last 1,000 fills, newest first: the server's history when the window opens, then each one as it
comes. Leg fills of spread orders are marked. Fills are matched by `seq`, so none shows twice.

## Positions

Positions by strategy, refreshed every 5 s and after fills.

## P&L

Total, realized and unrealized P&L over time, from when the window opens.

## Log

The main window's log of the last 500 entries: connection changes, order updates, and what you do
in any window. With the main window closed there is nothing to show.
