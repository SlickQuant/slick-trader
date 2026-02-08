#pragma once

#include <slick-trader/utils/types.h>
#include <vector>
#include <map>
#include <string>
#include <cstdint>

namespace slick::trader {

// Single price level in the order book
struct PriceLevel {
    price_t price;      // Price at this level (fixed-point)
    qty_t quantity;     // Total quantity at this price
    uint32_t num_orders; // Number of orders (if available)

    PriceLevel() : price(0), quantity(0), num_orders(0) {}
    PriceLevel(price_t p, qty_t q, uint32_t n = 0)
        : price(p), quantity(q), num_orders(n) {}
};

// Efficient order book implementation
// Uses std::map for sorted price levels (red-black tree)
// Optimized for: lookups, inserts, deletes at specific price levels
class OrderBook {
public:
    OrderBook(const std::string& instrument, uint32_t max_depth = 100)
        : instrument_(instrument)
        , max_depth_(max_depth)
        , last_update_time_(0)
        , sequence_number_(0) {}

    // Snapshot initialization (clears existing book)
    void apply_snapshot(const std::vector<PriceLevel>& bids,
                       const std::vector<PriceLevel>& asks,
                       uint64_t timestamp_ns,
                       uint64_t sequence = 0);

    // Incremental updates
    void update_bid(price_t price, qty_t quantity);
    void update_ask(price_t price, qty_t quantity);

    // Batch updates
    void update_bids(const std::vector<PriceLevel>& updates);
    void update_asks(const std::vector<PriceLevel>& updates);

    // Best bid/ask access
    bool get_best_bid(PriceLevel& level) const;
    bool get_best_ask(PriceLevel& level) const;

    double get_mid_price() const;
    double get_spread() const;
    double get_spread_bps() const;

    // Market depth queries
    const std::map<price_t, PriceLevel, std::greater<price_t>>& bids() const { return bids_; }
    const std::map<price_t, PriceLevel, std::less<price_t>>& asks() const { return asks_; }

    size_t bid_depth() const { return bids_.size(); }
    size_t ask_depth() const { return asks_.size(); }

    // Get top N levels
    std::vector<PriceLevel> get_top_bids(size_t n) const;
    std::vector<PriceLevel> get_top_asks(size_t n) const;

    // Volume calculations
    qty_t get_bid_volume(size_t levels = 5) const;
    qty_t get_ask_volume(size_t levels = 5) const;

    // VWAP calculations
    double get_bid_vwap(size_t levels = 5) const;
    double get_ask_vwap(size_t levels = 5) const;

    // Imbalance metrics
    double get_volume_imbalance(size_t levels = 5) const;  // (bid_vol - ask_vol) / (bid_vol + ask_vol)

    // Liquidity at price
    qty_t get_bid_quantity_at(price_t price) const;
    qty_t get_ask_quantity_at(price_t price) const;

    // State
    const std::string& instrument() const { return instrument_; }
    uint64_t last_update_time() const { return last_update_time_; }
    uint64_t sequence_number() const { return sequence_number_; }

    bool is_valid() const { return !bids_.empty() && !asks_.empty(); }

    // Clear the book
    void clear();

private:
    void trim_depth();

    std::string instrument_;
    uint32_t max_depth_;
    uint64_t last_update_time_;
    uint64_t sequence_number_;

    // Price levels sorted by price
    // Bids: descending order (highest price first)
    // Asks: ascending order (lowest price first)
    std::map<price_t, PriceLevel, std::greater<price_t>> bids_;
    std::map<price_t, PriceLevel, std::less<price_t>> asks_;
};

} // namespace slick::trader
