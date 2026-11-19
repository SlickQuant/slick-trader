# Dashboard Windows

Off production, an amber bar on top of every window names the config's `environment` (Sim, Cert,
NewRelease, ...), and the window title starts with it, as in `[Sim] Ladder`.

## Main window

- **Toolbar**: opens each tool. The engine's version shows next to the name, and the connection
  state on the right. The Log button shows a red count of errors logged while the Log window is
  closed.
- **Workspaces**: the menu next to the tools. Pick a workspace to close the tool windows and open
  its own where they were, ladders on their instruments. Save writes the open windows to the
  current one, and Save as to a new name or, once confirmed, an existing one. Delete works on any
  but `Default`. The main window opens the last used one as it starts, unless tool windows are up.
  Where the browser blocks windows (popups not allowed for the site), a dialog opens the rest, one
  per click.
- **Venues**: market data and order connection of each broker, live.
- **P&L totals**: total, realized and unrealized across strategies.
- **Strategies**: status (with the halt reason), P&L, start, stop, and a JSON editor for parameters.
  MANUAL, which always runs, has none of the buttons. A halt or pause shows within 5 s.

## Ladder

- A searchable instrument dropdown. Type to filter, then use the arrow keys and Enter. It shows the
  first 200 matches.
- A new ladder opens with nothing picked. A reloaded one, or one a workspace opened, comes back on
  its instrument.
- Uses the tick the broker defined. Without one it falls back to a small table, then 0.01.
- On CME and ICE the ladder keeps a row for each price with nothing on it, so rows stay put as
  levels come and go. A click on such a row trades at its price. The settings can show those rows
  everywhere or nowhere.
- ⚙ opens this ladder's settings: order confirmation, auto-center, volume bars, empty price rows
  and row height. They last through a reload and a workspace saves them with the ladder. A new
  ladder starts on the defaults. Auto-center, off at first, keeps the midpoint on the middle row
  and stops the wheel from scrolling. Off, the wheel scrolls and Space or a middle-click centers.
- Left-click a level to trade the qty there. Order confirmation is on by default, so the click
  first opens a dialog showing the order. Its Buy or Sell button, or Enter, sends it. Cancel,
  Escape or a click outside the dialog drops it. The dialog won't send in its first half second,
  so a double-click can't place the order. Middle and right clicks don't trade. The qty starts at
  the instrument's default: its
  `default_qty` in the ladder config, else the smallest order its broker takes, else the config's
  `default_qty`. A qty you type becomes that instrument's default. Until the config loads, the qty box
  is disabled and clicks send nothing.
- ▾ shows the qty, price and TIF boxes, hidden at first, and the ladder remembers the choice. A
  price you type overrides the level, and hiding the boxes clears it. The TIF starts at DAY on CME and
  ICE and GTC elsewhere. A market order left on GTC or DAY goes out IOC.
- Your resting orders are outlined in gold, kept through a reload and moved on a modify.
- Several ladders can run side by side, each streaming one book.

## Ticket

One order for any listed instrument, booked to MANUAL. The broker defaults to the one listing the
instrument. A `per_instrument` `default_broker_id` in `ladder_config` overrides it. The qty starts
at the instrument's default, as on the ladder, and the TIF at DAY on CME and ICE, GTC elsewhere.

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
