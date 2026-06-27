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

struct QuoteSlot {
    SlotState   state{SlotState::EMPTY};
    std::string client_id;       // assigned immediately by submit_order()
    std::string broker_id;       // assigned by broker in on_ack()
    double      posted_price{0.0};

    void reset() {
        state        = SlotState::EMPTY;
        client_id.clear();
        broker_id.clear();
        posted_price = 0.0;
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
                 "min_profit_usd={} taker_fee_rate={:.4f} size={} max_inv={} fv_mode={}",
                 instrument_, broker_str_, min_profit_usd_, taker_fee_rate_,
                 order_size_, max_inventory_, fair_value_mode_str_);
        return true;
    }

    void on_start() override {
        LOG_INFO("MarketMaker: Started");
    }

    void on_stop() override {
        LOG_INFO("MarketMaker: Stopping — cancelling open quotes");
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
        inventory_skew_factor_ = get_parameter<double>("inventory_skew_factor", 0.3);
        imbalance_sensitivity_ = get_parameter<double>("imbalance_sensitivity", 0.3);
        trend_sensitivity_     = get_parameter<double>("trend_sensitivity", 0.5);
        ema_fast_periods_      = get_parameter<int>("ema_fast_periods", 10);
        ema_slow_periods_      = get_parameter<int>("ema_slow_periods", 50);
        nlevel_depth_          = std::clamp(get_parameter<int>("nlevel_depth", 5),
                                            1, static_cast<int>(MAX_ORDER_BOOK_LEVELS));

        // Broker
        broker_str_ = get_parameter<std::string>("broker", "SIMULATED");
        if (broker_str_ == "COINBASE")      broker_id_ = BrokerId::COINBASE;
        else if (broker_str_ == "ALPACA")   broker_id_ = BrokerId::ALPACA;
        else                                broker_id_ = BrokerId::SIMULATED;

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
        if (is_running() && fair_value_ > 0.0) {
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

        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return;

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
        fair_value_     = (bid_vwap * ask_total_qty + ask_vwap * bid_total_qty) / total_qty;
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
            slot.state        = SlotState::ACTIVE;
            slot.broker_id    = std::string(event.order_id);
            slot.posted_price = from_price_t(event.price);
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
            if (is_running() && fair_value_ > 0.0) {
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
        LOG_WARN("MarketMaker: REJECT cid={} reason={}", event.client_order_id, event.reason);

        auto slot_matches = [&](const QuoteSlot& slot) -> bool {
            if (!cid.empty()) return cid == slot.client_id;
            return !oid.empty() && oid == slot.broker_id;
        };
        if (slot_matches(bid_slot_)) {
            bid_slot_.reset();
        } else if (slot_matches(ask_slot_)) {
            ask_slot_.reset();
        }
        // Next market data event will trigger re-quoting
    }

    void on_cancel(const OrderCancelEvent& event) override {
        const std::string_view cid(event.client_order_id);
        const std::string_view oid(event.order_id);
        LOG_DEBUG("MarketMaker: CANCEL cid={}", event.client_order_id);

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
        if (is_running() && fair_value_ > 0.0) {
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

    // Returns trend signal in [-1, +1]; 0 during EMA warm-up
    double trend_signal() const {
        if (quote_count_ < static_cast<uint64_t>(ema_slow_periods_) || fair_value_ <= 0.0) {
            return 0.0;
        }
        // Normalize EMA spread relative to fair value, then scale to [-1, +1]
        const double raw = (ema_fast_ - ema_slow_) / fair_value_;
        return std::clamp(raw * 1000.0, -1.0, 1.0);
    }

    double current_inventory() {
        if (!get_position_tracker()) return 0.0;
        return from_qty_t(
            get_position_tracker()->get_position(get_name(), instrument_).quantity);
    }

    bool needs_refresh(double posted, double target) const {
        if (posted <= 0.0) return true;
        return std::abs(target - posted) / posted > refresh_threshold_bps_;
    }

    // -----------------------------------------------------------------------
    // Core quoting logic
    // -----------------------------------------------------------------------

    void update_quotes() {
        if (!is_running() || fair_value_ <= 0.0) return;

        // Recover slots that got stuck in PENDING_NEW because submit_order() was
        // rejected before the order left the process (empty client_id = never sent).
        if (bid_slot_.state == SlotState::PENDING_NEW && bid_slot_.client_id.empty()) {
            bid_slot_.reset();
        }
        if (ask_slot_.state == SlotState::PENDING_NEW && ask_slot_.client_id.empty()) {
            ask_slot_.reset();
        }

        const double half_spread = min_profit_usd_ / (2.0 * order_size_) + fair_value_ * taker_fee_rate_;
        const double inventory    = current_inventory();
        const double inv_ratio    = std::clamp(inventory / max_inventory_, -1.0, 1.0);

        // Three independent additive adjustments to reservation price
        const double skew_offset = -inv_ratio * inventory_skew_factor_ * half_spread;
        const double trend_off   = trend_signal() * trend_sensitivity_ * half_spread;
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
            LOG_WARN("MarketMaker: {} order rejected pre-trade at {:.4f} — slot stays EMPTY",
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
        slot.state        = SlotState::PENDING_MODIFY;
        slot.posted_price = new_price;  // optimistic; confirmed to broker-actual in on_ack
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
    double inventory_skew_factor_{0.3};
    double imbalance_sensitivity_{0.3};
    double trend_sensitivity_{0.5};
    int    ema_fast_periods_{10};
    int    ema_slow_periods_{50};
    int    nlevel_depth_{5};

    // Derived from config
    double alpha_fast_{0.0};
    double alpha_slow_{0.0};

    // -----------------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------------
    double   fair_value_{0.0};
    double   book_imbalance_{0.0};

    // EMA state
    double   ema_fast_{0.0};
    double   ema_slow_{0.0};
    uint64_t quote_count_{0};

    // Quote slots (one per side)
    QuoteSlot bid_slot_;
    QuoteSlot ask_slot_;
};

// ---------------------------------------------------------------------------
// Plugin export
// ---------------------------------------------------------------------------
SLICK_TRADER_EXPORT_STRATEGY(MarketMaker)
