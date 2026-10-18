#pragma once

#include <slick-trader/core/position.hpp>
#include <slick-trader/core/position_store.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/fee_structure.hpp>
#include <slick/logger.hpp>
#include <array>
#include <unordered_map>
#include <string>
#include <vector>
#include <string_view>

namespace slick::trader {

// Single-writer position tracking with FIFO accounting
// Tracks positions per strategy and instrument
// NOTE: Single-writer only (called from StrategyManager thread)
// Readers use lock-free position_update_queue for consumption
class PositionTracker {
public:
    PositionTracker() = default;
    ~PositionTracker() = default;

    // Keeps every position in `store` from now on, and first takes up the positions it
    // holds - those of a previous run, with their open lots, so FIFO accounting carries on
    // where it stopped. Call it once, before any fill is booked and before any other thread
    // reads positions. Without a store positions live in memory only, and a restart starts
    // flat. The store must outlive the tracker.
    void attach_store(PositionStore& store);

    // Update position from a fill event
    // Uses FIFO accounting to calculate realized PnL
    void on_fill(const std::string& strategy_id, const OrderFillEvent& fill);

    // Whether the strategies have had this fill already. A broker that journals its fills
    // (OrderFillEvent::journal_seq) replays them after a restart, and those up to where the
    // strategies had got are to be dropped - every replayed one without a store, whose
    // positions started flat. False for a fill that was not journaled.
    bool fill_handled(const OrderFillEvent& fill) const noexcept {
        const auto broker = static_cast<std::size_t>(fill.broker_id);
        if (fill.journal_seq == 0) {
            return false;
        }
        if (fill.replayed && !store_) {
            return true;
        }
        return broker < journaled_.size() && fill.journal_seq <= journaled_[broker];
    }
    // The strategies have had the fill - every one that books it has booked it - so a
    // replay of it is dropped from now on, after a restart too when there is a store.
    void fill_dispatched(const OrderFillEvent& fill) noexcept;

    // Get current position for strategy/instrument (single-reader safe)
    Position get_position(const std::string& strategy_id, std::string_view instrument) const;

    // Get all positions for a strategy (single-reader safe)
    std::unordered_map<std::string, Position> get_all_positions(const std::string& strategy_id) const;

    // Get all positions across all strategies (single-reader safe)
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> get_all_positions() const;

    // Clear positions for a strategy
    void clear_strategy_positions(const std::string& strategy_id);

    // Clear all positions
    void clear_all();

    // Set fallback fee structure for brokers that don't report fees in fill events
    // Contract multiplier for an instrument - the currency value of a one-unit price move
    // on one unit of quantity (futures: ES = 50; spot instruments keep the default of 1).
    // Applies to later fills and to positions already held; PnL realized before it was set
    // stays as booked. Returns false, and changes nothing, for a non-finite or non-positive value.
    bool set_contract_multiplier(std::string_view instrument, double multiplier);
    double get_contract_multiplier(std::string_view instrument) const;

    void set_fee_structure(FeeStructure fee_structure) { fee_structure_ = fee_structure; }
    const FeeStructure& get_fee_structure() const { return fee_structure_; }

private:
    // FIFO lot for position tracking
    using Lot = PositionLot;

    // Position with FIFO lots for realized PnL calculation
    struct PositionWithLots {
        Position position;
        std::vector<Lot> lots;  // FIFO queue of entry lots
        uint32_t store_slot{PositionStore::NO_SLOT};  // where the store keeps it

        PositionWithLots() = default;
        explicit PositionWithLots(const std::string& inst) : position(inst) {}
    };

    // Records the position in the store, when there is one - with the fill it was booked
    // from, for that fill's journal number.
    void persist(const std::string& strategy_id, PositionWithLots& pos, const OrderFillEvent* fill = nullptr);

    // Apply fill to position using FIFO accounting
    void apply_fill(PositionWithLots& pos, bool is_buy, qty_t fill_qty, price_t fill_price, uint64_t timestamp, price_t fee_amount);

    // Calculate new average price when adding to position
    price_t calculate_new_avg_price(price_t current_avg, qty_t current_qty,
                                     price_t fill_price, qty_t fill_qty) const;

    FeeStructure fee_structure_;  // fallback for brokers without per-fill fee data

    PositionStore* store_{nullptr};  // non-owning; null keeps positions in memory only
    // By BrokerId: the journal number of the last fill the strategies have had.
    std::array<uint64_t, PositionStore::MAX_BROKERS> journaled_{};

    std::unordered_map<std::string, double> contract_multipliers_;  // instrument -> multiplier (absent = 1)

    // Positions: strategy_id -> instrument -> PositionWithLots
    std::unordered_map<std::string, std::unordered_map<std::string, PositionWithLots>> positions_;
};

} // namespace slick::trader
