// Coinbase's trades went out with no aggressor and is_buyer_maker backwards, so a ladder's LTQ cell
// couldn't say who crossed the spread. market_trades' side is the maker's, and the aggressor took
// the other one.
#include <gtest/gtest.h>
// The SDK first, as the connector's precompiled header has it.
#include <coinbase/market_data.hpp>
#include <coinbase/websocket.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick/queue.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <vector>

using namespace slick::trader;

TEST(CoinbaseTradesTest, ATradeNamesItsAggressorAsTheMakersOtherSide) {
    slick::stream_buffer_multiplexer mux(1024);
    slick::queue<uint8_t> control(4096);
    slick::queue<uint8_t> requests(4096);
    // Disabled, so it opens no socket. The test calls its trades callback directly.
    CoinbaseMDConnector connector({{"enabled", false}, {"queue_size", 1u << 16}, {"control_size", 4096}}, mux,
                                  control, requests);

    auto cursor = mux.initial_reading_index();   // where the bus stands: what is published next
    coinbase::MarketTrade bid_hit{"1", "BTC-USD", 0, 50000.0, 0.5, coinbase::Side::BUY};      // a resting buyer
    coinbase::MarketTrade ask_lifted{"2", "BTC-USD", 0, 50001.0, 0.25, coinbase::Side::SELL}; // a resting seller
    connector.onMarketTrades(nullptr, 0, {bid_hit, ask_lifted});

    std::vector<TradeEvent> trades;
    while (auto record = mux.read(cursor)) {
        const auto* event = reinterpret_cast<const Event*>(record.data);
        if (event->type == EventType::TRADE) {
            trades.push_back(*reinterpret_cast<const TradeEvent*>(event));
        }
    }
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].side, Side::SELL) << "a seller hit the resting bid";
    EXPECT_TRUE(trades[0].is_buyer_maker);
    EXPECT_EQ(trades[0].quantity, to_qty_t(0.5));
    EXPECT_EQ(trades[1].side, Side::BUY) << "a buyer lifted the resting offer";
    EXPECT_FALSE(trades[1].is_buyer_maker);
}
