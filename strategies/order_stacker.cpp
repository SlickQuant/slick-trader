// Order Stacker Strategy
//
// Maintains a two-sided ladder ("stack") of resting post-only orders offset a
// configurable number of ticks behind the top of book, using the header-only
// slick-stacker library (slick::stacker::stacker_pair).
//
// With offset_ticks = 0, levels = 4, level_gap_ticks = 1, tick_size = 0.01:
//
//     ask ladder                      bid ladder
//     100.04                          100.00   <- best_bid, level 0
//     100.03                           99.99
//     100.02                           99.98
//     100.01                           99.97
//     100.00  <- best_ask, level 0     99.96
//
// You give the stacker one price and one quantity per side; it lays down the
// rungs behind that and keeps the venue in agreement as acknowledgements,
// fills, cancels and rejects come back.
//
// Config parameters (all under "parameters" in the strategy JSON):
//   instrument           (default: "BTC-USD") — Coinbase uses product ids, Hyperliquid bare symbols
//   broker               (default: "COINBASE") — "COINBASE", "HYPERLIQUID", "ALPACA"
//   tick_size            (default: 0.01) — the STACK tick; a multiple of the venue tick.
//                                          Must equal the venue tick for offset_ticks=0 to sit
//                                          exactly on the touch (see snap_to_grid note below).
//   offset_ticks         (default: 0)    — ticks behind the touch for level 0; 0 quotes AT the touch
//   levels               (default: 4)    — rungs BEHIND level 0; level 0 is not counted,
//                                          so levels=4 means 5 working prices per side
//   level_gap_ticks      (default: 1)    — tick spacing between rungs
//   stack_qty            (default: 0.001) — per-rung size
//   top_qty              (default: 0 → stack_qty) — size of level 0
//   max_position         (default: 0.01) — pull a side once the filled position reaches this
//   position_resume_qty  (default: 0 → stack_qty) — hysteresis band before the side resumes
//   post_only            (default: true)
//   prefer_modify        (default: true) — reprice in place rather than cancel/replace
//   refill_on_fill       (default: false)
//   slack_levels         (default: 0)    — rungs past the bottom kept rather than cancelled
//   min_order_qty        (default: 0)    — dust guard after partial fills
//
// Design invariants — each is load-bearing, do not "simplify" them away:
//   1. Executor::place() inserts its record only AFTER submit_order() returns.
//      submit_order() can synthesise a reject and call on_order_reject() on us
//      synchronously, i.e. re-enter from inside reconcile(). An unresolvable id
//      makes that callback a no-op.
//   2. on_ack() stores the broker order_id BEFORE calling into the stacker.
//      on_accepted() -> drain_pending() can call Executor::cancel() synchronously.
//   3. Quote prices are snapped to the tick grid. An off-grid price makes
//      level_ring::depth_of() return k_invalid_depth, which prepare_window()
//      answers with a full rebase — cancelling every order, every tick.
//   4. cfg.ack_required is hardcoded true. cancel()/modify() need the broker
//      order_id, which only exists after the ack; ack_required=true is what
//      stops the stacker asking for them any earlier.

#include <slick/stacker/stacker_pair.hpp>

#include <slick/logger.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_export.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/core/position.hpp>
#include <slick-trader/utils/types.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace slick::trader;
namespace stk = slick::stacker;

// ---------------------------------------------------------------------------
// Compile-time sizing
// ---------------------------------------------------------------------------

// level_ring indexes levels_[depth & mask] — a sliding modular window, not a
// fixed price range — and would_overflow() compares only the WIDTH of the live
// band. A steadily trending price therefore costs nothing. What the capacity
// must cover is the transient band during a jump, when the old ladder is still
// live (its cancels in flight) while the new one is placed:
//
//   band ~= 2 * (offset_ticks + levels * level_gap_ticks) + jump_between_reconciles
//
// 2048 gives ~$20 of jump tolerance at a $0.01 tick. Exceeding it is survivable
// — rebase() cancels everything and re-establishes — but rebase_count() should
// stay at 1 in steady state.
struct stacker_traits {
    static constexpr std::uint16_t level_capacity = 2048;  // must be a power of two
    static constexpr std::uint16_t max_orders     = 64;    // levels+1 per side, with headroom
    static constexpr std::uint16_t max_levels     = 64;
};

// ---------------------------------------------------------------------------
// Strategy
// ---------------------------------------------------------------------------

class OrderStacker : public StrategyBase {
public:
    // Must not log and must not throw: SLICK_TRADER_EXPORT_STRATEGY default-
    // constructs two extra instances as function-local statics, possibly before
    // set_logger_instance() has run.
    OrderStacker()
        : StrategyBase("OrderStacker", "1.0.0") {}

    ~OrderStacker() override = default;

private:
    // -----------------------------------------------------------------------
    // Order bookkeeping
    // -----------------------------------------------------------------------

    enum class RecState : std::uint8_t {
        PENDING_NEW,     // submitted, no ack yet
        LIVE,            // acknowledged and resting
        PENDING_MODIFY,  // modify outstanding
        PENDING_CANCEL   // cancel outstanding
    };

    struct Rec {
        std::string  client_order_id;    // known immediately from submit_order()
        std::string  broker_order_id;    // learned in on_ack()
        stk::side_t  side{stk::side_t::buy};
        stk::price_t req_price{0};       // last price asked for (grid-aligned)
        stk::qty_t   req_qty{0};
        RecState     state{RecState::PENDING_NEW};
        bool         cancel_when_acked{false};
    };

    // The order-entry interface slick-stacker drives. A nested class is a member
    // of OrderStacker, so it reaches StrategyBase's protected submit_order() /
    // cancel_order() / modify_order() directly — no friend declaration and no
    // std::function indirection, so the calls inline.
    //
    // None of place/modify/cancel may call back into the stacker.
    class Executor {
    public:
        using order_id_t = std::uint64_t;
        static constexpr order_id_t invalid_order_id = 0;

        explicit Executor(OrderStacker* owner) noexcept : owner_(owner) {}

        order_id_t place(stk::side_t side, stk::price_t price, stk::qty_t qty,
                         stk::order_type_t type);
        bool       modify(const order_id_t& id, stk::price_t price, stk::qty_t qty);
        bool       cancel(const order_id_t& id);

        // Ids are (seq << 1) | side, so the side survives the record being
        // erased. 0 stays reserved as invalid_order_id.
        static constexpr order_id_t make_id(std::uint64_t seq, stk::side_t s) noexcept {
            return (seq << 1) | static_cast<std::uint64_t>(s);
        }
        static constexpr stk::side_t side_of(order_id_t id) noexcept {
            return static_cast<stk::side_t>(id & 1u);
        }

    private:
        OrderStacker* owner_;
    };

    friend class Executor;

    // Snapshot of the JSON parameters, handed from the WebServer thread to the
    // worker thread. Plain scalars only.
    struct Params {
        std::string instrument{"BTC-USD"};
        std::string broker{"COINBASE"};
        double      tick_size{0.01};
        int         offset_ticks{0};
        int         levels{4};
        int         level_gap_ticks{1};
        double      stack_qty{0.001};
        double      top_qty{0.0};
        double      max_position{0.01};
        double      position_resume_qty{0.0};
        bool        post_only{true};
        bool        prefer_modify{true};
        bool        refill_on_fill{false};
        int         slack_levels{0};
        double      min_order_qty{0.0};
    };

protected:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    bool on_initialize() override {
        session_stamp_ = get_timestamp_ns();
        // on_config() has already run (StrategyBase::initialize calls it first),
        // so the pending params are populated. Apply them here so instrument_
        // and broker_id_ exist before get_subscriptions() is called.
        apply_pending_params();
        if (!configured_) {
            LOG_ERROR("OrderStacker: initial configuration is invalid — refusing to start");
            return false;
        }
        return true;
    }

    void on_start() override {
        LOG_INFO("OrderStacker: Started");
    }

    void on_stop() override {
        // StrategyManager::stop() calls this before joining the worker thread,
        // so it runs concurrently with dispatch. pair_ is worker-thread-owned
        // and must not be touched here — cancel through the base class only.
        LOG_INFO("OrderStacker: Stopping — cancelling working ladder");
        cancel_all_orders();
    }

    void on_shutdown() override {
        LOG_INFO("OrderStacker: Shutdown");
    }

    // Reached from the WebServer thread via PUT /api/strategies/:id/config, so
    // it only latches. apply_pending_params() does the real work on the worker.
    void on_config(const nlohmann::json& /*params*/) override {
        Params p;
        p.instrument          = get_parameter<std::string>("instrument", "BTC-USD");
        p.broker              = get_parameter<std::string>("broker", "COINBASE");
        p.tick_size           = get_parameter<double>("tick_size", 0.01);
        p.offset_ticks        = get_parameter<int>("offset_ticks", 0);
        p.levels              = get_parameter<int>("levels", 4);
        p.level_gap_ticks     = get_parameter<int>("level_gap_ticks", 1);
        p.stack_qty           = get_parameter<double>("stack_qty", 0.001);
        p.top_qty             = get_parameter<double>("top_qty", 0.0);
        p.max_position        = get_parameter<double>("max_position", 0.01);
        p.position_resume_qty = get_parameter<double>("position_resume_qty", 0.0);
        p.post_only           = get_parameter<bool>("post_only", true);
        p.prefer_modify       = get_parameter<bool>("prefer_modify", true);
        p.refill_on_fill      = get_parameter<bool>("refill_on_fill", false);
        p.slack_levels        = get_parameter<int>("slack_levels", 0);
        p.min_order_qty       = get_parameter<double>("min_order_qty", 0.0);

        {
            std::lock_guard<std::mutex> lk(params_mutex_);
            pending_ = p;
        }
        params_dirty_.store(true, std::memory_order_release);
    }

    // The base implementation stamps get_timestamp_ns() (steady_clock, ~100 ns
    // granularity on Windows). One reconcile() places levels+1 orders in a tight
    // loop, so collisions are plausible — and a collision would make
    // orders_by_client_id_.emplace() silently drop the second order while the
    // broker still receives it. A monotonic counter cannot collide.
    std::string generate_order_id() override {
        return get_name() + "_" + std::to_string(session_stamp_) + "_"
             + std::to_string(++cid_seq_);
    }

public:
    // -----------------------------------------------------------------------
    // Subscriptions
    // -----------------------------------------------------------------------

    // LEVEL2 is what makes StrategyManager synthesise QuoteEvents from the
    // internal book. TRADE is a second pump() heartbeat, so a refused message
    // gets retried on an instrument that is trading but not re-quoting.
    std::vector<SubscriptionRequest> get_subscriptions() const override {
        return {
            SubscriptionRequest(broker_id_, SubscriptionType::LEVEL2, {instrument_}),
            SubscriptionRequest(broker_id_, SubscriptionType::TRADE,  {instrument_})
        };
    }

    // -----------------------------------------------------------------------
    // Market data
    // -----------------------------------------------------------------------

    void on_quote(const QuoteEvent& event) override {
        // Market data is broadcast to every loaded strategy.
        if (std::string_view(event.instrument) != instrument_) return;

        apply_pending_params();
        if (!configured_ || get_position_tracker() == nullptr) return;

        const bool bid_ok = event.bid_price != NULL_PRICE && event.bid_price > 0;
        const bool ask_ok = event.ask_price != NULL_PRICE && event.ask_price > 0;

        // Publish the market's top of book first: it feeds apply_cross_guard(),
        // which stacker_pair runs at both ends of reconcile().
        pair_.on_top_of_book(bid_ok ? event.bid_price : stk::k_null_price,
                             ask_ok ? event.ask_price : stk::k_null_price);

        // Unusable book, or the engine has halted us — withdraw everything.
        // This bail is also what makes offset_ticks=0 safe: it removes the
        // locked/crossed case, which is the only one where quoting at the touch
        // could trip the stacker's cross guard.
        if (!is_running() || !bid_ok || !ask_ok || event.bid_price >= event.ask_price) {
            pair_.pull();
            pump();
            return;
        }

        // Snap the touch onto the stack grid, rounding AWAY from the market on
        // both sides, then step off by whole ticks. Two guarantees:
        //   (a) both prices are exact multiples of tick_, so depth_of() never
        //       returns k_invalid_depth and we never trigger a spurious rebase;
        //   (b) rounding can never move a price across the spread.
        // When tick_ equals the venue tick these are identity, so offset_ticks=0
        // lands exactly on the touch. With a coarser tick_ the quote snaps to
        // the grid and therefore sits up to one tick behind it.
        const stk::price_t bid_px = floor_tick(event.bid_price) - offset_ticks_ * tick_;
        const stk::price_t ask_px = ceil_tick(event.ask_price) + offset_ticks_ * tick_;
        if (bid_px <= 0) {
            pair_.pull();
            pump();
            return;
        }

        const qty_t pos = get_position_tracker()->get_position(get_name(), instrument_).quantity;
        update_guard_latches(pos);

        if (buy_pulled_)  pair_.buy().pull();  else pair_.buy().quote(bid_px, top_qty_);
        if (sell_pulled_) pair_.sell().pull(); else pair_.sell().quote(ask_px, top_qty_);

        pump();
        maybe_log_health();
    }

    void on_trade(const TradeEvent& event) override {
        if (std::string_view(event.instrument) != instrument_) return;
        apply_pending_params();
        // Retry heartbeat: reconcile() leaves dirty_ set when a message was
        // refused, and nothing else will wake us.
        if (configured_) pump();
    }

protected:
    // -----------------------------------------------------------------------
    // Order lifecycle
    // -----------------------------------------------------------------------

    void on_ack(const OrderAckEvent& event) override {
        auto [id, rec] = resolve(event.client_order_id, event.order_id);
        if (rec == nullptr) return;

        // MUST happen before calling into the stacker: on_accepted() ->
        // on_confirmed() -> drain_pending() can call Executor::cancel()
        // synchronously, and that needs the broker order_id.
        if (rec->broker_order_id.empty() && event.order_id[0] != '\0') {
            rec->broker_order_id.assign(event.order_id);
            oid_to_local_.emplace(rec->broker_order_id, id);
        }

        // An off-grid acked price would make apply_contribution() silently drop
        // the quantity from level accounting, and the add pass would then
        // over-quote. Fall back to what we asked for.
        const stk::price_t px = on_grid(event.price) ? event.price : rec->req_price;
        const stk::qty_t   qy = event.quantity > 0   ? event.quantity : rec->req_qty;

        // New-vs-modify is decided from our own state, not the event: a modify
        // ack is byte-identical in shape to a new-order ack.
        const bool was_modify = (rec->state == RecState::PENDING_MODIFY);
        rec->state     = RecState::LIVE;
        rec->req_price = px;
        rec->req_qty   = qy;

        if (was_modify) pair_.on_replaced(rec->side, id, px, qy);
        else            pair_.on_accepted(rec->side, id, px, qy);

        // A rebase() cancels every slot ignoring ack_required, so it can ask to
        // cancel an order we had no broker id for. This is that deferred cancel.
        if (rec->cancel_when_acked) {
            rec->cancel_when_acked = false;
            cancel_order(rec->broker_order_id, rec->client_order_id);
            rec->state = RecState::PENDING_CANCEL;
        }

        pump();
    }

    void on_fill(const OrderFillEvent& event) override {
        auto [id, rec] = resolve(event.client_order_id, event.order_id);
        if (rec == nullptr) return;

        const stk::side_t side = rec->side;
        const bool done = (event.remaining_quantity <= 0);

        // k_null_price on purpose: the stacker then uses its own on-grid
        // acked_price for consume_target(). Forwarding a raw venue fill price
        // risks an off-grid consume_target() that silently does nothing.
        // fill_quantity is this fill alone, which is what on_filled wants.
        pair_.on_filled(side, id, event.fill_quantity, stk::k_null_price);
        if (done) erase_rec(id);

        pump();
    }

    void on_cancel(const OrderCancelEvent& event) override {
        auto [id, rec] = resolve(event.client_order_id, event.order_id);
        if (rec == nullptr) return;   // duplicate cancel

        const stk::side_t side = rec->side;
        const auto why = (rec->state == RecState::PENDING_CANCEL)
                       ? stk::cancel_reason_t::requested
                       : stk::cancel_reason_t::unsolicited;

        // Always 0 = "whatever was left". A positive value smaller than what is
        // resting is read as a partial trim and would leave the order working.
        pair_.on_canceled(side, id, 0, why);
        erase_rec(id);

        pump();
    }

    void on_reject(const OrderRejectEvent& event) override {
        auto [id, rec] = resolve(event.client_order_id, event.order_id);
        // Also the path taken by the synthetic pre-trade reject raised from
        // inside submit_order(): no record exists yet, so this returns.
        if (rec == nullptr) return;

        const stk::side_t side = rec->side;
        const auto why = map_reject(event.reason);

        switch (rec->state) {
        case RecState::PENDING_MODIFY:
            pair_.on_modify_rejected(side, id, why);
            rec->state = RecState::LIVE;
            break;
        case RecState::PENDING_CANCEL:
            pair_.on_cancel_rejected(side, id, why);
            rec->state = RecState::LIVE;
            break;
        default:
            // Includes post-only rejects, which are routine at offset_ticks=0:
            // the stacker retires the slot and the add pass re-places it.
            LOG_DEBUG("OrderStacker: order {} rejected: {}", rec->client_order_id, event.reason);
            pair_.on_rejected(side, id, why);
            erase_rec(id);
            break;
        }

        pump();
    }

private:
    // -----------------------------------------------------------------------
    // Reconcile driver
    // -----------------------------------------------------------------------

    // slick-stacker's on_* handlers never send; all I/O happens in reconcile().
    // The library wants one reconcile per event batch, but slick-trader
    // dispatches one event at a time with no batch boundary — so we reconcile at
    // the end of every handler and accept the extra passes. reconcile() returns
    // immediately when not dirty, and with ack_required=true convergence is
    // driven by the ack events anyway.
    void pump() {
        if (in_pump_) return;   // structurally prevents re-entering reconcile()
        in_pump_ = true;
        for (int pass = 0; pass < 4; ++pass) {
            pair_.reconcile();
            if (orphan_cancels_.empty()) break;
            // Confirmations the executor owed but could not raise from inside
            // cancel(), because it must not call back into the stacker.
            for (auto id : orphan_cancels_) {
                pair_.on_canceled(Executor::side_of(id), id, 0,
                                  stk::cancel_reason_t::unsolicited);
            }
            orphan_cancels_.clear();
        }
        in_pump_ = false;
    }

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void apply_pending_params() {
        if (!params_dirty_.exchange(false, std::memory_order_acquire)) return;

        Params p;
        {
            std::lock_guard<std::mutex> lk(params_mutex_);
            p = pending_;
        }

        // Instrument and broker are baked into the subscriptions, which are
        // fixed once the strategy starts.
        if (!configured_) {
            instrument_ = p.instrument;
            broker_str_ = p.broker;
            broker_id_  = to_broker_id(broker_str_);
            if (broker_id_ == BrokerId::UNKNOWN) {
                LOG_ERROR("OrderStacker: unknown broker '{}'", broker_str_);
                return;
            }
        } else if (p.instrument != instrument_ || p.broker != broker_str_) {
            LOG_WARN("OrderStacker: instrument/broker cannot change at runtime "
                     "(subscriptions are fixed at start) — keeping {}/{}",
                     instrument_, broker_str_);
        }

        if (p.offset_ticks < 0) {
            LOG_WARN("OrderStacker: offset_ticks {} is negative — clamping to 0", p.offset_ticks);
            p.offset_ticks = 0;
        }

        const stk::stacker_config cfg = build_config(p);
        if (const auto err = cfg.validate<stacker_traits>(); err != stk::config_error::ok) {
            LOG_ERROR("OrderStacker: rejecting configuration — {}", stk::to_string(err));
            return;   // keep the previous, working configuration
        }

        if (configured_ && cfg.tick_size != tick_) {
            LOG_WARN("OrderStacker: tick_size changed — the price grid will be rebuilt "
                     "and every working order cancelled");
        }

        pair_.buy().configure(cfg);
        pair_.sell().configure(cfg);

        tick_         = cfg.tick_size;
        offset_ticks_ = p.offset_ticks;
        top_qty_      = p.top_qty > 0.0 ? to_qty_t(p.top_qty) : cfg.stack_qty;
        max_position_ = to_qty_t(p.max_position);
        resume_qty_   = p.position_resume_qty > 0.0 ? to_qty_t(p.position_resume_qty)
                                                    : cfg.stack_qty;
        post_only_    = p.post_only;
        configured_   = true;

        const int span = offset_ticks_ + cfg.levels * cfg.level_gap_ticks;
        LOG_INFO("OrderStacker: configured instrument={} broker={} tick={} offset_ticks={} "
                 "levels={} gap={} stack_qty={} top_qty={} max_position={} span={} ticks",
                 instrument_, broker_str_, from_price_t(tick_), offset_ticks_,
                 cfg.levels, cfg.level_gap_ticks, from_qty_t(cfg.stack_qty),
                 from_qty_t(top_qty_), from_qty_t(max_position_), span);

        if (span * 2 > stacker_traits::level_capacity / 2) {
            LOG_WARN("OrderStacker: ladder span {} ticks is large relative to level_capacity {} "
                     "— expect frequent rebases", span, stacker_traits::level_capacity);
        }
    }

    stk::stacker_config build_config(const Params& p) const {
        stk::stacker_config cfg;
        cfg.tick_size       = to_price_t(p.tick_size);
        cfg.levels          = static_cast<std::uint16_t>(
                                  std::clamp(p.levels, 0, static_cast<int>(stacker_traits::max_levels)));
        cfg.level_gap_ticks = static_cast<std::uint16_t>(std::max(p.level_gap_ticks, 1));
        cfg.stack_qty       = to_qty_t(std::max(p.stack_qty, 0.0));
        cfg.min_order_qty   = to_qty_t(std::max(p.min_order_qty, 0.0));
        cfg.slack_levels    = static_cast<std::uint16_t>(std::max(p.slack_levels, 0));
        cfg.prefer_modify   = p.prefer_modify;
        cfg.refill_on_fill  = p.refill_on_fill;
        // Not a tuning knob: cancel()/modify() need the broker order_id, which
        // only exists after the ack. ack_required=true is what guarantees the
        // stacker never asks for them before then.
        cfg.ack_required    = true;
        cfg.order_type      = stk::order_type_t::limit;
        return cfg;
    }

    // -----------------------------------------------------------------------
    // Position guard
    // -----------------------------------------------------------------------

    // Latched with a hysteresis band: a bare comparison would flap, pulling and
    // re-placing a whole ladder on alternating fills.
    //
    // Note the guard sees only the FILLED position, so peak exposure can
    // overshoot max_position by up to one ladder ((levels+1) * stack_qty)
    // before the pull lands. Size max_position and the risk-limit
    // max_position_size accordingly.
    void update_guard_latches(qty_t pos) {
        const qty_t resume_at = max_position_ > resume_qty_ ? max_position_ - resume_qty_ : 0;

        if (!buy_pulled_ && pos >= max_position_) {
            buy_pulled_ = true;
            LOG_WARN("OrderStacker: position {} at long limit {} — pulling BUY ladder",
                     from_qty_t(pos), from_qty_t(max_position_));
        } else if (buy_pulled_ && pos <= resume_at) {
            buy_pulled_ = false;
            LOG_INFO("OrderStacker: position {} back inside — resuming BUY ladder",
                     from_qty_t(pos));
        }

        if (!sell_pulled_ && pos <= -max_position_) {
            sell_pulled_ = true;
            LOG_WARN("OrderStacker: position {} at short limit {} — pulling SELL ladder",
                     from_qty_t(pos), from_qty_t(max_position_));
        } else if (sell_pulled_ && pos >= -resume_at) {
            sell_pulled_ = false;
            LOG_INFO("OrderStacker: position {} back inside — resuming SELL ladder",
                     from_qty_t(pos));
        }
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    // Prices are positive, so integer division truncates toward zero == floor.
    stk::price_t floor_tick(stk::price_t p) const noexcept { return (p / tick_) * tick_; }
    stk::price_t ceil_tick(stk::price_t p) const noexcept  { return ((p + tick_ - 1) / tick_) * tick_; }
    bool on_grid(stk::price_t p) const noexcept { return tick_ > 0 && p > 0 && (p % tick_) == 0; }

    Rec* find_rec(Executor::order_id_t id) {
        auto it = recs_.find(id);
        return it != recs_.end() ? &it->second : nullptr;
    }

    // Resolve an inbound event to a local id. The client_order_id path is
    // normal; the order_id fallback covers brokers (Coinbase) that send fills
    // and cancels with an empty client_order_id. Failing to resolve is also the
    // ownership filter — order events are broadcast to every strategy.
    std::pair<Executor::order_id_t, Rec*> resolve(const char* cid, const char* oid) {
        if (cid != nullptr && cid[0] != '\0') {
            auto it = cid_to_local_.find(std::string(cid));
            if (it != cid_to_local_.end()) return {it->second, find_rec(it->second)};
        }
        if (oid != nullptr && oid[0] != '\0') {
            auto it = oid_to_local_.find(std::string(oid));
            if (it != oid_to_local_.end()) return {it->second, find_rec(it->second)};
        }
        return {Executor::invalid_order_id, nullptr};
    }

    void erase_rec(Executor::order_id_t id) {
        auto it = recs_.find(id);
        if (it == recs_.end()) return;
        cid_to_local_.erase(it->second.client_order_id);
        if (!it->second.broker_order_id.empty()) {
            oid_to_local_.erase(it->second.broker_order_id);
        }
        recs_.erase(it);
    }

    // on_rejected() and on_modify_rejected() ignore the reason entirely; only
    // on_cancel_rejected() inspects it, and only for too_late_to_act. So a
    // coarse mapping is enough.
    static stk::reject_reason_t map_reject(const char* reason) {
        if (reason == nullptr) return stk::reject_reason_t::unknown;
        std::string r(reason);
        std::transform(r.begin(), r.end(), r.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (r.find("not found") != std::string::npos ||
            r.find("already")   != std::string::npos ||
            r.find("too late")  != std::string::npos) {
            return stk::reject_reason_t::too_late_to_act;
        }
        if (r.find("rate limit") != std::string::npos ||
            r.find("throttle")   != std::string::npos) {
            return stk::reject_reason_t::throttled;
        }
        if (r.find("disconnect") != std::string::npos ||
            r.find("disabled")   != std::string::npos) {
            return stk::reject_reason_t::retryable;
        }
        if (r.find("post only") != std::string::npos ||
            r.find("post-only") != std::string::npos) {
            return stk::reject_reason_t::self_match_prevention;
        }
        return stk::reject_reason_t::unknown;
    }

    void maybe_log_health() {
        if (++quote_count_ % 1000 != 0) return;
        LOG_DEBUG("OrderStacker: health rebase(buy/sell)={}/{} blocked={}/{} live={}/{} recs={}",
                  pair_.buy().rebase_count(),  pair_.sell().rebase_count(),
                  pair_.buy().blocked_count(), pair_.sell().blocked_count(),
                  pair_.buy().live_order_count(), pair_.sell().live_order_count(),
                  recs_.size());
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    // Resolved configuration (worker thread only)
    std::string  instrument_{"BTC-USD"};
    std::string  broker_str_{"COINBASE"};
    BrokerId     broker_id_{BrokerId::COINBASE};
    stk::price_t tick_{0};
    int          offset_ticks_{0};
    stk::qty_t   top_qty_{0};
    qty_t        max_position_{0};
    qty_t        resume_qty_{0};
    bool         post_only_{true};
    bool         configured_{false};

    // Config handoff (WebServer thread -> worker thread)
    std::mutex        params_mutex_;
    Params            pending_;
    std::atomic<bool> params_dirty_{false};

    // Order bookkeeping
    std::unordered_map<Executor::order_id_t, Rec>         recs_;
    std::unordered_map<std::string, Executor::order_id_t> cid_to_local_;
    std::unordered_map<std::string, Executor::order_id_t> oid_to_local_;
    std::uint64_t next_seq_{0};
    std::uint64_t session_stamp_{0};
    std::uint64_t cid_seq_{0};
    std::uint64_t quote_count_{0};

    // Pump machinery
    std::vector<Executor::order_id_t> orphan_cancels_;
    bool buy_pulled_{false};
    bool sell_pulled_{false};
    bool in_pump_{false};

    // MUST be the last two members, in this order: stacker_pair is neither
    // copyable nor movable and holds an Executor&, so the executor has to be
    // constructed first and both have to live in place.
    Executor                                    exec_{this};
    stk::stacker_pair<Executor, stacker_traits> pair_{exec_, stk::stacker_config{}};
};

// ---------------------------------------------------------------------------
// Executor — defined out of line, where OrderStacker is complete
// ---------------------------------------------------------------------------

inline OrderStacker::Executor::order_id_t
OrderStacker::Executor::place(stk::side_t side, stk::price_t price, stk::qty_t qty,
                              stk::order_type_t /*type*/) {
    OrderStacker& o = *owner_;

    // ORDERING IS LOAD-BEARING. submit_order() runs pre-trade risk and broker
    // connectivity checks, and on failure synthesises an OrderRejectEvent and
    // calls on_order_reject() on us SYNCHRONOUSLY — i.e. re-entering from
    // inside stacker::reconcile(). The record is inserted only after it
    // returns, so that callback cannot resolve an id, returns immediately, and
    // therefore cannot re-enter the stacker.
    const std::string cid = o.submit_order(
        o.instrument_, o.broker_id_,
        side == stk::side_t::buy ? Side::BUY : Side::SELL,
        static_cast<qty_t>(qty),
        OrderType::LIMIT,
        static_cast<price_t>(price),
        TimeInForce::GTC,
        o.post_only_);

    if (cid.empty()) {
        // Pre-trade reject or broker down. Not an error the stacker propagates:
        // it stops adding at this level and retries on the next reconcile.
        return invalid_order_id;
    }

    const order_id_t id = make_id(++o.next_seq_, side);
    Rec rec;
    rec.client_order_id = cid;
    rec.side            = side;
    rec.req_price       = price;
    rec.req_qty         = qty;
    rec.state           = RecState::PENDING_NEW;
    o.cid_to_local_.emplace(cid, id);
    o.recs_.emplace(id, std::move(rec));
    return id;
}

inline bool OrderStacker::Executor::modify(const order_id_t& id, stk::price_t price,
                                           stk::qty_t qty) {
    OrderStacker& o = *owner_;
    Rec* rec = o.find_rec(id);
    if (rec == nullptr) return false;

    // The wire message carries the broker order_id. Refusing leaves the
    // stacker's accounting untouched so it retries, which is strictly better
    // than reporting a message we did not send.
    if (rec->broker_order_id.empty()) return false;

    o.modify_order(rec->client_order_id, static_cast<price_t>(price), static_cast<qty_t>(qty));
    rec->req_price = price;
    rec->req_qty   = qty;
    rec->state     = RecState::PENDING_MODIFY;
    return true;
}

inline bool OrderStacker::Executor::cancel(const order_id_t& id) {
    OrderStacker& o = *owner_;
    Rec* rec = o.find_rec(id);

    if (rec == nullptr) {
        // Already terminal on our side. Report the message as sent and let
        // pump() raise the confirmation once reconcile() has returned — we must
        // not call back into the stacker from here.
        o.orphan_cancels_.push_back(id);
        return true;
    }

    if (rec->broker_order_id.empty()) {
        // Only reachable from rebase(), which cancels every slot ignoring
        // ack_required. Latch it and fire from on_ack(); returning false keeps
        // the stacker dirty so it retries as well.
        rec->cancel_when_acked = true;
        return false;
    }

    o.cancel_order(rec->broker_order_id, rec->client_order_id);   // broker id FIRST
    rec->state = RecState::PENDING_CANCEL;
    return true;
}

SLICK_TRADER_EXPORT_STRATEGY(OrderStacker)
