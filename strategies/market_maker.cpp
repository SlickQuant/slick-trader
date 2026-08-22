// Market Maker Strategy
//
// A production-ready market making strategy with:
//   - Configurable fair value: SIMPLE_MID, WEIGHTED_MID, or NLEVEL_WMID
//   - Inventory skew: reservation price shifted against open position
//   - Trend detection: dual-EMA signal shifts quotes in trend direction
//   - Book imbalance: immediate microstructure pressure adjustment
//   - Quote refresh: direct order modify (single round-trip, no quote gap)
//   - State machine per quote slot (EMPTY/PENDING_NEW/ACTIVE/PENDING_MODIFY/PENDING_CANCEL)
//   - Post-only GTC orders only (maker-friendly)
//
// Config parameters (all under "parameters" in strategy JSON):
//   instrument             (default: "BTC-USD")
//   broker                 (default: "SIMULATED") — "COINBASE", "ALPACA", "SIMULATED"
//   min_profit_usd         (default: 1.0)  — minimum dollar profit per round trip after fees;
//                                             actual half_spread = min_profit/(2*qty) + mid*broker_rate
//   order_size             (default: 0.001) — order size per side
//   max_inventory          (default: 0.01)  — absolute position limit; skips a side at limit
//   refresh_threshold_bps  (default: 0.5)  — reprice if target moves more than this many bps
//   tick_size              (default: 0)    — instrument tick; enables the absolute threshold below
//   refresh_threshold_ticks(default: 0)    — reprice if target moves more than this many ticks.
//                                             Overrides refresh_threshold_bps when both this and
//                                             tick_size are set. Required for instruments trading
//                                             at or through zero, where bps of price means nothing.
//   allow_negative_price   (default: false)— accept zero/negative bids and asks as real prices
//                                             (futures calendar spreads) instead of bad data.
//                                             Pair it with the tick threshold above.
//   inventory_skew_factor  (default: 0.3)  — [0,1] how aggressively to lean vs inventory
//   fair_value_mode        (default: "WEIGHTED_MID") — see above
//   nlevel_depth           (default: 5)    — book levels for NLEVEL_WMID (max 10)
//   ema_fast_periods       (default: 10)   — fast EMA window
//   ema_slow_periods       (default: 50)   — slow EMA window (warm-up before trend applied)
//   trend_sensitivity      (default: 0.5)  — [0,1] scaling for trend offset
//   imbalance_sensitivity  (default: 0.3)  — [0,1] scaling for book imbalance offset

#include <slick/logger.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_export.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/core/position.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/utils/types.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>

using namespace slick::trader;

// ---------------------------------------------------------------------------
// Supporting types
// ---------------------------------------------------------------------------

enum class FairValueMode : uint8_t {
    SIMPLE_MID,    // (bid + ask) / 2
    WEIGHTED_MID,  // (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
    NLEVEL_WMID    // N-level volume-weighted, cross-side weighted
};

enum class SlotState : uint8_t {
    EMPTY,           // No order on this side
    PENDING_NEW,     // submit_order() called, waiting for on_ack
    ACTIVE,          // Acknowledged by broker, resting in book
    PENDING_MODIFY,  // modify_order() called, waiting for on_ack
    PENDING_CANCEL   // cancel_order() called (inventory limit), waiting for on_cancel
};

// "No price" marker for the double-valued prices below. Zero cannot serve as one: futures
// calendar spreads and the odd outright settle at or below zero, so 0.0 and negatives are
// real prices. NaN is what from_price_t(NULL_PRICE) already yields, so the two agree.
inline constexpr double kNoPrice = std::numeric_limits<double>::quiet_NaN();

// How far two prices may sit apart and still count as the same price, in absolute price units.
//
//   tick mode (tick_tolerance > 0) — a fixed distance, independent of price level. The only
//     measure that means anything for an instrument that trades at or through zero, such as a
//     futures calendar spread, where a percentage of the price is a percentage of nothing.
//   bps mode  — a fraction of |reference|. The absolute value matters: a spread quoted below
//     zero with a signed reference yields a negative tolerance, and every comparison against
//     it inverts, so every move reads as "unchanged" and the strategy stops repricing.
inline double price_tolerance(double reference, double bps_fraction, double tick_tolerance) {
    if (tick_tolerance > 0.0) return tick_tolerance;
    return std::abs(reference) * bps_fraction;
}

struct QuoteSlot {
    SlotState   state{SlotState::EMPTY};
    std::string client_id;       // assigned immediately by submit_order()
    std::string broker_id;       // assigned by broker in on_ack()
    // Last state the venue confirmed for this order. Updated on every ack, so a rejected
    // amendment falls back to what is actually resting rather than to what we last asked for.
    double      posted_price{kNoPrice};
    double      posted_qty{0.0};
    double      pending_price{kNoPrice};  // price requested by an in-flight modify

    void reset() {
        state         = SlotState::EMPTY;
        client_id.clear();
        broker_id.clear();
        posted_price  = kNoPrice;
        posted_qty    = 0.0;
        pending_price = kNoPrice;
    }
};

// ---------------------------------------------------------------------------
// Strategy
// ---------------------------------------------------------------------------

class MarketMaker : public StrategyBase {
public:
    MarketMaker()
        : StrategyBase("MarketMaker", "1.0.0") {}

    ~MarketMaker() override = default;

protected:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    bool on_initialize() override {
        LOG_INFO("MarketMaker: Initialized instrument={} broker={} "
                 "min_profit_usd={} taker_fee_rate={:.4f} size={} max_inv={} fv_mode={} "
                 "refresh={} neg_price={}",
                 instrument_, broker_str_, min_profit_usd_, taker_fee_rate_,
                 order_size_, max_inventory_, fair_value_mode_str_,
                 refresh_mode_description(), allow_negative_price_);
        return true;
    }

    std::string refresh_mode_description() const {
        return tick_tolerance_ > 0.0
            ? std::format("{}tick ({})", refresh_threshold_ticks_, tick_tolerance_)
            : std::format("{}bps", refresh_threshold_bps_ * 10000.0);
    }

    void on_start() override {
        LOG_INFO("MarketMaker: Started");
    }

    void on_stop() override {
        LOG_INFO("MarketMaker: Stopping - cancelling open quotes");
        // Explicitly cancel ACTIVE and PENDING_MODIFY slots (PENDING_MODIFY may not
        // be covered by cancel_all_orders() if PENDING_MODIFY is not in is_open()).
        for (auto* slot : {&bid_slot_, &ask_slot_}) {
            if ((slot->state == SlotState::ACTIVE ||
                 slot->state == SlotState::PENDING_MODIFY) &&
                !slot->broker_id.empty()) {
                cancel_order(slot->broker_id, slot->client_id);
            }
        }
        cancel_all_orders();  // catch PENDING_NEW orders tracked in base class
    }

    void on_shutdown() override {
        LOG_INFO("MarketMaker: Shutdown");
    }

    void on_config(const nlohmann::json& /*params*/) override {
        instrument_    = get_parameter<std::string>("instrument", "BTC-USD");
        min_profit_usd_ = get_parameter<double>("min_profit_usd", 1.0);
        order_size_    = get_parameter<double>("order_size", 0.001);
        max_inventory_ = get_parameter<double>("max_inventory", 0.01);
        refresh_threshold_bps_ = get_parameter<double>("refresh_threshold_bps", 0.5) / 10000.0;

        // Absolute reprice threshold. Both have to be set for it to take over from bps; a bps
        // threshold is a fraction of the price and therefore meaningless for an instrument that
        // trades at or through zero.
        tick_size_               = get_parameter<double>("tick_size", 0.0);
        refresh_threshold_ticks_ = get_parameter<double>("refresh_threshold_ticks", 0.0);
        tick_tolerance_ = (tick_size_ > 0.0 && refresh_threshold_ticks_ > 0.0)
                        ? tick_size_ * refresh_threshold_ticks_
                        : 0.0;

        allow_negative_price_ = get_parameter<bool>("allow_negative_price", false);
        if (allow_negative_price_ && tick_tolerance_ <= 0.0) {
            LOG_WARN("MarketMaker: allow_negative_price is set but no tick threshold is configured "
                     "(tick_size + refresh_threshold_ticks). Near zero a bps threshold collapses "
                     "to nothing and the strategy will reprice on every tick.");
        }
        inventory_skew_factor_ = get_parameter<double>("inventory_skew_factor", 0.3);
        imbalance_sensitivity_ = get_parameter<double>("imbalance_sensitivity", 0.3);
        trend_sensitivity_     = get_parameter<double>("trend_sensitivity", 0.5);
        ema_fast_periods_      = get_parameter<int>("ema_fast_periods", 10);
        ema_slow_periods_      = get_parameter<int>("ema_slow_periods", 50);
        nlevel_depth_          = std::clamp(get_parameter<int>("nlevel_depth", 5),
                                            1, static_cast<int>(MAX_ORDER_BOOK_LEVELS));

        // Broker
        broker_str_ = get_parameter<std::string>("broker", "SIMULATED");
        broker_id_ = to_broker_id(broker_str_);

        // Fair value mode
        fair_value_mode_str_ = get_parameter<std::string>("fair_value_mode", "WEIGHTED_MID");
        if (fair_value_mode_str_ == "SIMPLE_MID")       fair_value_mode_ = FairValueMode::SIMPLE_MID;
        else if (fair_value_mode_str_ == "NLEVEL_WMID") fair_value_mode_ = FairValueMode::NLEVEL_WMID;
        else {
            fair_value_mode_str_ = "WEIGHTED_MID";
            fair_value_mode_ = FairValueMode::WEIGHTED_MID;
        }

        // Pull taker fee rate from broker (set in PositionTracker by TradingEngine at startup)
        if (auto* pt = get_position_tracker()) {
            taker_fee_rate_ = pt->get_fee_structure().rate;
        }

        // Derive EMA smoothing factors
        alpha_fast_ = 2.0 / (ema_fast_periods_ + 1.0);
        alpha_slow_ = 2.0 / (ema_slow_periods_ + 1.0);

        // If already running with a valid fair value, immediately recalculate quotes so
        // updated parameters (spread, size, etc.) take effect without waiting for the next tick.
        // update_quotes() uses needs_refresh() → modify_slot() for in-place order updates.
        if (is_running() && !std::isnan(fair_value_)) {
            LOG_INFO("MarketMaker: Config updated at runtime — recalculating quotes");
            update_quotes();
        }
    }

    // -----------------------------------------------------------------------
    // Subscriptions — dynamic based on fair value mode
    // -----------------------------------------------------------------------

    std::vector<SubscriptionRequest> get_subscriptions() const override {
        return {
            SubscriptionRequest(broker_id_, SubscriptionType::LEVEL2, {instrument_}),
            SubscriptionRequest(broker_id_, SubscriptionType::TRADE, {instrument_})
        };
    }

    // -----------------------------------------------------------------------
    // Market data handlers
    // -----------------------------------------------------------------------

    void on_quote(const QuoteEvent& event) override {
        if (std::string_view(event.instrument) != instrument_) return;

        const double bid     = from_price_t(event.bid_price);
        const double ask     = from_price_t(event.ask_price);
        const double bid_qty = from_qty_t(event.bid_quantity);
        const double ask_qty = from_qty_t(event.ask_quantity);

        if (!is_usable_price(bid) || !is_usable_price(ask) || bid >= ask) return;

        const double total_qty = bid_qty + ask_qty;

        if (fair_value_mode_ == FairValueMode::SIMPLE_MID || total_qty <= 0.0) {
            fair_value_     = (bid + ask) / 2.0;
            book_imbalance_ = 0.0;
        } else {
            // WEIGHTED_MID: weight toward the thinner side
            fair_value_     = (bid * ask_qty + ask * bid_qty) / total_qty;
            book_imbalance_ = (bid_qty - ask_qty) / total_qty;
        }

        update_ema(fair_value_);
        update_quotes();
    }

    void on_order_book(const slick::orderbook::OrderBookL2& book) override {
        if (fair_value_mode_ != FairValueMode::NLEVEL_WMID) return;

        const int depth_bid = std::min(static_cast<int>(book.levelCount(slick::orderbook::Side::Buy)), nlevel_depth_);
        const int depth_ask = std::min(static_cast<int>(book.levelCount(slick::orderbook::Side::Sell)), nlevel_depth_);
        if (depth_bid == 0 || depth_ask == 0) return;

        double bid_notional = 0.0, bid_total_qty = 0.0;
        double ask_notional = 0.0, ask_total_qty = 0.0;

        for (int i = 0; i < depth_bid; ++i) {
            auto *bid = book.getLevelByIndex(slick::orderbook::Side::Buy, i);
            const double p  = from_price_t(bid->price);
            const double q  = from_qty_t(bid->quantity);
            bid_notional  += p * q;
            bid_total_qty += q;
        }
        for (int i = 0; i < depth_ask; ++i) {
            auto *ask = book.getLevelByIndex(slick::orderbook::Side::Sell, i);
            const double p  = from_price_t(ask->price);
            const double q  = from_qty_t(ask->quantity);
            ask_notional  += p * q;
            ask_total_qty += q;
        }

        if (bid_total_qty <= 0.0 || ask_total_qty <= 0.0) return;

        const double bid_vwap  = bid_notional / bid_total_qty;
        const double ask_vwap  = ask_notional / ask_total_qty;
        const double total_qty = bid_total_qty + ask_total_qty;

        // Cross-side weighted: weight toward the thinner side (same logic as WEIGHTED_MID)
        const double fv = (bid_vwap * ask_total_qty + ask_vwap * bid_total_qty) / total_qty;

        // Individual levels are not screened the way on_quote() screens top of book, so a level
        // carrying NULL_PRICE would poison the VWAP with NaN. Check the result before adopting it.
        if (!is_usable_price(fv)) return;

        fair_value_     = fv;
        book_imbalance_ = (bid_total_qty - ask_total_qty) / total_qty;

        update_ema(fair_value_);
        update_quotes();
    }

    void on_trade(const TradeEvent& /*event*/) override {
        // Trades update mark price via StrategyManager automatically
    }

    // -----------------------------------------------------------------------
    // Order lifecycle
    // -----------------------------------------------------------------------

    void on_ack(const OrderAckEvent& event) override {
        const std::string_view cid(event.client_order_id);
        // Handles both PENDING_NEW → ACTIVE and PENDING_MODIFY → ACTIVE transitions.
        // event.price is the broker-confirmed price (new price for modifies).
        auto handle = [&](QuoteSlot& slot, const char* label) {
            if (cid != slot.client_id) return;

            const double acked_price = from_price_t(event.price);
            const double acked_qty   = from_qty_t(event.quantity);

            // Every ack is the venue stating where the order rests right now, so the slot
            // records it unconditionally — including acks that do not settle a pending
            // edit. That recorded state is exactly what the order falls back to if the
            // edit is rejected, and on venues that allow several edits in flight (CME) it
            // can be a *later* accepted one than the edit being waited on, not the state
            // we started from. Only the state transition below is conditional.
            slot.broker_id  = std::string(event.order_id);
            slot.posted_qty = acked_qty;
            if (!std::isnan(acked_price)) {
                slot.posted_price = acked_price;  // keep the last real price if this ack has none
            }

            // While an edit is in flight the venue also keeps streaming the order's
            // *pre-edit* state (Coinbase publishes an OPEN update per book change).
            // Completing the modify on one of those drops the slot to ACTIVE at the old
            // price, so the next tick fires a second edit on top of the first — which is
            // what the exchange answers with edit_failure_reason: UNKNOWN. Only an ack at
            // the price we asked for closes a PENDING_MODIFY.
            if (slot.state == SlotState::PENDING_MODIFY && !is_confirming_price(acked_price, slot.pending_price)) {
                LOG_DEBUG("MarketMaker: {} resting at {:.4f}x{} while modify to {:.4f} is in "
                          "flight — recorded, slot stays PENDING_MODIFY",
                          label, acked_price, acked_qty, slot.pending_price);
                return;
            }

            slot.state         = SlotState::ACTIVE;
            slot.pending_price = kNoPrice;
            LOG_DEBUG("MarketMaker: {} ack price={:.4f} broker={}", label,
                      slot.posted_price, event.order_id);
        };
        handle(bid_slot_, "BID");
        handle(ask_slot_, "ASK");
    }

    void on_fill(const OrderFillEvent& event) override {
        const std::string_view cid(event.client_order_id);
        const std::string_view oid(event.order_id);
        const bool full_fill = from_qty_t(event.remaining_quantity) <= 0.0;

        LOG_INFO("MarketMaker: FILL {} {} {}@{:.4f} remaining={}",
                 event.instrument,
                 event.side == Side::BUY ? "BUY" : "SELL",
                 from_qty_t(event.fill_quantity),
                 from_price_t(event.fill_price),
                 from_qty_t(event.remaining_quantity));

        if (full_fill) {
            // Coinbase WebSocket fill events often omit client_order_id; fall back to broker_id.
            auto slot_matches = [&](const QuoteSlot& slot) -> bool {
                if (!cid.empty()) return cid == slot.client_id;
                return !oid.empty() && oid == slot.broker_id;
            };
            if (slot_matches(bid_slot_)) {
                bid_slot_.reset();
            } else if (slot_matches(ask_slot_)) {
                ask_slot_.reset();
            }
            // Re-quote immediately on full fill
            if (is_running() && !std::isnan(fair_value_)) {
                update_quotes();
            }
        }
        // Partial fill: slot stays ACTIVE, order still resting

        if (get_pnl_calculator()) {
            auto pnl = get_pnl_calculator()->calculate_pnl(get_name(), get_timestamp_ns());
            LOG_INFO("MarketMaker: PnL realized={:.4f} unrealized={:.4f} total={:.4f}",
                     pnl.get_realized(), pnl.get_unrealized(), pnl.get_total());
        }
    }

    void on_reject(const OrderRejectEvent& event) override {
        const std::string_view cid(event.client_order_id);
        const std::string_view oid(event.order_id);
        LOG_WARN("MarketMaker: REJECT cid={} reason={}", cid, event.reason);

        auto slot_matches = [&](const QuoteSlot& slot) -> bool {
            if (!cid.empty()) return cid == slot.client_id;
            return !oid.empty() && oid == slot.broker_id;
        };

        // A refused *amendment* is not a dead order. An edit or a cancel the venue rejects
        // leaves the original resting at the last state it confirmed, so the slot returns to
        // ACTIVE and the next tick retries. Clearing it is what leaked an order per failed
        // edit: the slot went EMPTY and update_quotes() placed a second one beside the live
        // original. Every other reject — a PENDING_NEW that never reached the book, a venue
        // FAILED on a resting order — does mean the order is gone, so the slot is cleared.
        auto handle = [&](QuoteSlot& slot, const char* label) -> bool {
            if (!slot_matches(slot)) return false;
            switch (slot.state) {
            case SlotState::PENDING_MODIFY:
            case SlotState::PENDING_CANCEL:
                LOG_WARN("MarketMaker: {} {} rejected — order still resting at {:.4f}x{}, "
                         "the last state the venue confirmed", label,
                         slot.state == SlotState::PENDING_MODIFY ? "edit" : "cancel",
                         slot.posted_price, slot.posted_qty);
                slot.state         = SlotState::ACTIVE;
                slot.pending_price = kNoPrice;
                break;
            default:
                slot.reset();
                break;
            }
            return true;
        };
        if (!handle(bid_slot_, "BID")) {
            handle(ask_slot_, "ASK");
        }
        // Next market data event will trigger re-quoting
    }

    void on_cancel(const OrderCancelEvent& event) override {
        const std::string_view cid(event.client_order_id);
        const std::string_view oid(event.order_id);
        LOG_DEBUG("MarketMaker: CANCEL cid={}", cid);

        auto slot_matches = [&](const QuoteSlot& slot) -> bool {
            if (!cid.empty()) return cid == slot.client_id;
            return !oid.empty() && oid == slot.broker_id;
        };
        if (slot_matches(bid_slot_)) {
            bid_slot_.reset();
        } else if (slot_matches(ask_slot_)) {
            ask_slot_.reset();
        }

        // Re-quote if strategy is still running (i.e., this was a refresh cancel)
        if (is_running() && !std::isnan(fair_value_)) {
            update_quotes();
        }
    }

private:
    // -----------------------------------------------------------------------
    // Signal helpers
    // -----------------------------------------------------------------------

    void update_ema(double value) {
        if (quote_count_ == 0) {
            ema_fast_ = value;
            ema_slow_ = value;
        } else {
            ema_fast_ = alpha_fast_ * value + (1.0 - alpha_fast_) * ema_fast_;
            ema_slow_ = alpha_slow_ * value + (1.0 - alpha_slow_) * ema_slow_;
        }
        ++quote_count_;
    }

    // Returns trend signal in [-1, +1]; 0 during EMA warm-up.
    //
    // The EMA spread is an absolute price move, so it needs a scale to become a signal.
    // |fair value| / 1000 is the long-standing one — a 0.1% spread between the EMAs saturates
    // the signal — and the absolute value keeps it from inverting below zero. That is no scale
    // at all for an instrument trading at or near zero, so tick mode normalises by the half
    // spread instead: one half-spread of EMA drift saturates.
    double trend_signal(double half_spread) const {
        if (quote_count_ < static_cast<uint64_t>(ema_slow_periods_) || std::isnan(fair_value_)) {
            return 0.0;
        }
        const double scale = tick_tolerance_ > 0.0 ? half_spread
                                                   : std::abs(fair_value_) / 1000.0;
        if (scale <= 0.0) return 0.0;
        return std::clamp((ema_fast_ - ema_slow_) / scale, -1.0, 1.0);
    }

    double current_inventory() {
        if (!get_position_tracker()) return 0.0;
        return from_qty_t(
            get_position_tracker()->get_position(get_name(), instrument_).quantity);
    }

    double tolerance_around(double reference) const {
        return price_tolerance(reference, refresh_threshold_bps_, tick_tolerance_);
    }

    bool needs_refresh(double posted, double target) const {
        if (std::isnan(posted)) return true;   // nothing confirmed yet — post one
        if (std::isnan(target)) return false;  // no usable target — leave the order alone
        return std::abs(target - posted) > tolerance_around(posted);
    }

    // Does an ack price confirm the edit we requested? The venue rounds to its own tick,
    // so an ack never comes back bit-exact. Half the refresh threshold is the right
    // window: it swallows tick rounding, yet is strictly narrower than any reprice we
    // would have asked for, since needs_refresh() only fires past the full threshold —
    // so a pre-edit price can never be mistaken for a confirmation.
    bool is_confirming_price(double acked, double requested) const {
        if (std::isnan(requested)) return true;  // no edit outstanding — nothing to confirm
        if (std::isnan(acked)) return false;     // venue quoted no price — keep waiting
        return std::abs(acked - requested) <= tolerance_around(requested) * 0.5;
    }

    // A price the strategy can quote around. NaN is the venue saying "no price". Zero and
    // negatives are real prices only where the instrument actually trades there, so they stay
    // rejected as bad data unless allow_negative_price says otherwise.
    bool is_usable_price(double p) const {
        if (std::isnan(p)) return false;
        return allow_negative_price_ || p > 0.0;
    }

    // -----------------------------------------------------------------------
    // Core quoting logic
    // -----------------------------------------------------------------------

    void update_quotes() {
        if (!is_running() || std::isnan(fair_value_)) return;

        // Recover slots that got stuck in PENDING_NEW because submit_order() was
        // rejected before the order left the process (empty client_id = never sent).
        if (bid_slot_.state == SlotState::PENDING_NEW && bid_slot_.client_id.empty()) {
            bid_slot_.reset();
        }
        if (ask_slot_.state == SlotState::PENDING_NEW && ask_slot_.client_id.empty()) {
            ask_slot_.reset();
        }

        // Fees are charged on absolute notional, so the fee term uses |fair value| — a signed
        // one would shrink the spread on an instrument quoted below zero.
        const double half_spread = min_profit_usd_ / (2.0 * order_size_)
                                 + std::abs(fair_value_) * taker_fee_rate_;
        const double inventory    = current_inventory();
        const double inv_ratio    = std::clamp(inventory / max_inventory_, -1.0, 1.0);

        // Three independent additive adjustments to reservation price
        const double skew_offset = -inv_ratio * inventory_skew_factor_ * half_spread;
        const double trend_off   = trend_signal(half_spread) * trend_sensitivity_ * half_spread;
        const double imb_off     = book_imbalance_ * imbalance_sensitivity_ * half_spread;

        const double adjusted_mid = fair_value_ + skew_offset + trend_off + imb_off;
        const double bid_target   = adjusted_mid - half_spread;
        const double ask_target   = adjusted_mid + half_spread;

        LOG_DEBUG("MarketMaker: fv={:.4f} skew={:.5f} trend={:.5f} imb={:.5f} "
                  "bid={:.4f} ask={:.4f} inv={:.6f}",
                  fair_value_, skew_offset, trend_off, imb_off,
                  bid_target, ask_target, inventory);

        // ---- Bid side ----
        // Check post-order inventory so we never submit an order the risk manager
        // will reject (which would lock the slot with an empty client_id).
        switch (bid_slot_.state) {
        case SlotState::EMPTY:
            if (inventory + order_size_ < max_inventory_) {
                place_quote(Side::BUY, bid_target, bid_slot_);
            }
            break;
        case SlotState::ACTIVE:
            if (inventory >= max_inventory_) {
                cancel_slot(bid_slot_);                        // over limit — remove entirely
            } else if (needs_refresh(bid_slot_.posted_price, bid_target)) {
                modify_slot(bid_slot_, bid_target);            // stale price — reprice in place
            }
            break;
        default:
            break; // PENDING_NEW, PENDING_MODIFY, PENDING_CANCEL — wait for event
        }

        // ---- Ask side ----
        switch (ask_slot_.state) {
        case SlotState::EMPTY:
            if (inventory - order_size_ > -max_inventory_) {
                place_quote(Side::SELL, ask_target, ask_slot_);
            }
            break;
        case SlotState::ACTIVE:
            if (inventory <= -max_inventory_) {
                cancel_slot(ask_slot_);                        // over limit — remove entirely
            } else if (needs_refresh(ask_slot_.posted_price, ask_target)) {
                modify_slot(ask_slot_, ask_target);            // stale price — reprice in place
            }
            break;
        default:
            break;
        }
    }

    void place_quote(Side side, double price, QuoteSlot& slot) {
            auto& br = BrokerStatusRegistry::instance();
        if (!br.is_order_enabled(broker_id_) || !br.is_order_connected(broker_id_)) {
            if (!order_unavailable_logged_) {
                LOG_WARN("MarketMaker: {} order system unavailable for broker {} - skipping quote",
                         side == Side::BUY ? "BID" : "ASK", broker_str_);
                order_unavailable_logged_ = true;
            }
            return;
        } else if (order_unavailable_logged_) {
            order_unavailable_logged_ = false;
        }

        const auto cid = submit_order(
            instrument_,
            broker_id_,
            side,
            to_qty_t(order_size_),
            OrderType::LIMIT,
            to_price_t(price),
            TimeInForce::GTC,
            /*post_only=*/true
        );

        if (cid.empty()) {
            // submit_order() rejected pre-trade (risk check); slot stays EMPTY so
            // update_quotes() will retry on the next market data tick.
            LOG_WARN("MarketMaker: {} order rejected pre-trade at {:.4f} - slot stays EMPTY",
                     side == Side::BUY ? "BID" : "ASK", price);
            return;
        }

        slot.state        = SlotState::PENDING_NEW;
        slot.client_id    = cid;
        slot.broker_id    = "";
        slot.posted_price = price;

        LOG_INFO("MarketMaker: {} {} @ {:.4f} cid={}",
                 side == Side::BUY ? "BID" : "ASK",
                 order_size_, price, cid);
    }

    void cancel_slot(QuoteSlot& slot) {
        if (slot.state == SlotState::ACTIVE && !slot.broker_id.empty()) {
            cancel_order(slot.broker_id, slot.client_id);
            slot.state = SlotState::PENDING_CANCEL;
        }
    }

    void modify_slot(QuoteSlot& slot, double new_price) {
        if (slot.state != SlotState::ACTIVE) return;
        modify_order(slot.client_id, to_price_t(new_price), to_qty_t(order_size_));
        slot.state         = SlotState::PENDING_MODIFY;
        slot.pending_price = new_price;
        // posted_price deliberately keeps the last confirmed price: until the venue acks
        // the edit that is still where the order rests, and if the edit is rejected
        // needs_refresh() has to compare the next target against reality, not against a
        // price we only wished for.
        LOG_DEBUG("MarketMaker: MODIFY {} -> {:.4f}", slot.client_id, new_price);
    }

    // -----------------------------------------------------------------------
    // Config state
    // -----------------------------------------------------------------------
    std::string   instrument_{"BTC-USD"};
    std::string   broker_str_{"SIMULATED"};
    BrokerId      broker_id_{BrokerId::SIMULATED};
    FairValueMode fair_value_mode_{FairValueMode::WEIGHTED_MID};
    std::string   fair_value_mode_str_{"WEIGHTED_MID"};

    double min_profit_usd_{1.0};
    double taker_fee_rate_{0.0};
    double order_size_{0.001};
    double max_inventory_{0.01};
    double refresh_threshold_bps_{0.5 / 10000.0};
    double tick_size_{0.0};
    double refresh_threshold_ticks_{0.0};
    bool   allow_negative_price_{false};
    double inventory_skew_factor_{0.3};
    double imbalance_sensitivity_{0.3};
    double trend_sensitivity_{0.5};
    int    ema_fast_periods_{10};
    int    ema_slow_periods_{50};
    int    nlevel_depth_{5};

    // Derived from config
    double alpha_fast_{0.0};
    double alpha_slow_{0.0};
    // > 0 switches the reprice threshold from bps of price to this absolute distance.
    double tick_tolerance_{0.0};

    // -----------------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------------
    double   fair_value_{kNoPrice};
    double   book_imbalance_{0.0};

    // EMA state
    double   ema_fast_{0.0};
    double   ema_slow_{0.0};
    uint64_t quote_count_{0};

    // Quote slots (one per side)
    QuoteSlot bid_slot_;
    QuoteSlot ask_slot_;

    bool order_unavailable_logged_{false};  // avoid spamming logs if broker order system is down
};

// ---------------------------------------------------------------------------
// Plugin export
// ---------------------------------------------------------------------------
SLICK_TRADER_EXPORT_STRATEGY(MarketMaker)
