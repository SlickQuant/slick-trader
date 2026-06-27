#include <slick-trader/brokers/hyperliquid/hyperliquid_md_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/utils/time.hpp>
#include <hyperliquid/info.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace slick::trader {

static constexpr uint32_t producer_id = static_cast<uint8_t>(BrokerId::HYPERLIQUID) * 2 - 1;

HyperliquidMDConnector::HyperliquidMDConnector(
    const json& config,
    slick::stream_buffer_multiplexer &mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& request_queue
)
    : enabled_(config.empty() ? false : config.value("enabled", true))
    , control_queue_(control_queue)
    , request_queue_(request_queue)
    , request_cursor_(request_queue.initial_reading_index())
    , api_url_(config.value("api_url", "https://api.hyperliquid.xyz"))
{
    if (enabled_) {
        event_buffer_ = mux.add_producer(
            producer_id,
            config.value("queue_size", 1u << 26),
            config.value("control_size", 1u << 24),
            config.contains("shm_name") ? config["shm_name"].get_ref<const std::string&>().c_str() : nullptr
        );
    }
}

HyperliquidMDConnector::~HyperliquidMDConnector() {
    stop();
}

void HyperliquidMDConnector::register_md_connector() {
    if(enabled_) {
        EventSerializer::publish<RegisterEvent>(control_queue_, [=](RegisterEvent& event) {
            event.broker_id = BrokerId::HYPERLIQUID;
            event.component_type = ComponentType::MARKET_DATA;
            event.producer_id = producer_id;
        });
    }
}

bool HyperliquidMDConnector::connect() {
    if (!enabled_) {
        return false;
    }

    LOG_INFO("Hyperliquid MD: Connecting to {}", api_url_);

    try {
        info_ = std::make_unique<hyperliquid::Info>(api_url_, false, true);
        subscribe_all();
        connected_.store(true, std::memory_order_release);
        LOG_INFO("Hyperliquid MD: Connected");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: connect() failed: {}", e.what());
        info_.reset();
        connected_.store(false, std::memory_order_release);
        return false;
    }
}

void HyperliquidMDConnector::disconnect() {
    if (info_) {
        LOG_INFO("Hyperliquid MD: Disconnecting");
        // Unsubscribe all channels before destroying Info
        for (auto& [sub_json_str, sub_id] : sub_ids_) {
            try {
                info_->unsubscribe(nlohmann::json::parse(sub_json_str), sub_id);
            } catch (...) {}
        }
        sub_ids_.clear();
        info_.reset();
        connected_.store(false, std::memory_order_release);
        LOG_INFO("Hyperliquid MD: Disconnected");
    }
}

bool HyperliquidMDConnector::subscribe(const MDSubscription& request) {
    LOG_INFO("Hyperliquid MD: Subscribing to {} instruments (type: {})",
             request.num_instruments, static_cast<int>(request.subscription_type));

    switch (request.subscription_type) {
        case SubscriptionType::TRADE:
            for (auto i = 0u; i < request.num_instruments; ++i) {
                trade_instruments_.emplace_back(request.instruments[i]);
            }
            break;
        case SubscriptionType::QUOTE:
            for (auto i = 0u; i < request.num_instruments; ++i) {
                quote_instruments_.emplace_back(request.instruments[i]);
            }
            break;
        case SubscriptionType::LEVEL2:
        case SubscriptionType::ORDER_BOOK:
            for (auto i = 0u; i < request.num_instruments; ++i) {
                level2_instruments_.emplace_back(request.instruments[i]);
            }
            level2_depth_ = request.depth;
            break;
    }

    // If already connected, subscribe immediately
    if (info_) {
        for (auto i = 0u; i < request.num_instruments; ++i) {
            const auto& coin = request.instruments[i];
            nlohmann::json sub;
            std::string ch_type;

            switch (request.subscription_type) {
                case SubscriptionType::TRADE:
                    ch_type = "trades";
                    sub = {{"type", "trades"}, {"coin", coin}};
                    break;
                case SubscriptionType::QUOTE:
                    ch_type = "bbo";
                    sub = {{"type", "bbo"}, {"coin", coin}};
                    break;
                case SubscriptionType::LEVEL2:
                case SubscriptionType::ORDER_BOOK:
                    ch_type = "l2Book";
                    sub = {{"type", "l2Book"}, {"coin", coin}};
                    break;
            }

            int id = info_->subscribe(sub, [this, ch_type](const nlohmann::json& msg) {
                if (ch_type == "trades") on_trades(msg);
                else if (ch_type == "bbo") on_bbo(msg);
                else on_l2book(msg);
            });
            sub_ids_.emplace_back(sub.dump(), id);
        }
    }

    return true;
}

bool HyperliquidMDConnector::unsubscribe(const MDSubscription& request) {
    auto remove_from = [&](std::vector<std::string>& list) {
        for (auto i = 0u; i < request.num_instruments; ++i) {
            const auto& ins = request.instruments[i];
            list.erase(std::remove(list.begin(), list.end(), ins), list.end());
        }
    };

    switch (request.subscription_type) {
        case SubscriptionType::TRADE:  remove_from(trade_instruments_); break;
        case SubscriptionType::QUOTE:  remove_from(quote_instruments_); break;
        case SubscriptionType::LEVEL2:
        case SubscriptionType::ORDER_BOOK: remove_from(level2_instruments_); break;
    }

    return true;
}

void HyperliquidMDConnector::start() {
    if (running_.load(std::memory_order_relaxed)) {
        LOG_WARN("Hyperliquid MD: Already running");
        return;
    }
    running_.store(true, std::memory_order_release);
    connect();
    worker_thread_ = std::thread(&HyperliquidMDConnector::run, this);
}

void HyperliquidMDConnector::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);
    disconnect();
}

void HyperliquidMDConnector::run() {
    LOG_INFO("Hyperliquid MD: Worker thread started");
    connect();

    while (running_.load(std::memory_order_acquire)) {
        info_->dispatch();
    }

    LOG_INFO("Hyperliquid MD: Worker thread stopped");
}

void HyperliquidMDConnector::subscribe_all() {
    auto subscribe_coin_list = [&](const std::vector<std::string>& coins,
                                   const std::string& ch_type) {
        for (const auto& coin : coins) {
            nlohmann::json sub = {{"type", ch_type}, {"coin", coin}};
            int id = info_->subscribe(sub, [this, ch_type](const nlohmann::json& msg) {
                if (ch_type == "trades") on_trades(msg);
                else if (ch_type == "bbo") on_bbo(msg);
                else on_l2book(msg);
            });
            sub_ids_.emplace_back(sub.dump(), id);
        }
    };

    subscribe_coin_list(trade_instruments_, "trades");
    subscribe_coin_list(quote_instruments_, "bbo");
    subscribe_coin_list(level2_instruments_, "l2Book");
}

// ── Private event handlers ────────────────────────────────────────────────────

void HyperliquidMDConnector::on_trades(const nlohmann::json& msg) {
    const auto data_ptr = msg.find("data");
    if (data_ptr == msg.end() || !data_ptr->is_array()) return;

    for (const auto& trade : *data_ptr) {
        try {
            const std::string coin = trade.at("coin").get<std::string>();
            const double px = std::stod(trade.at("px").get<std::string>());
            const double sz = std::stod(trade.at("sz").get<std::string>());
            const std::string side_str = trade.value("side", "B");
            const uint64_t tid = trade.value("tid", uint64_t(0));
            const uint64_t time_ms = trade.value("time", uint64_t(0));

            EventSerializer::publish<TradeEvent>(*event_buffer_, [&](TradeEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.event_time_ns = time_ms * 1'000'000ULL;
                ev.set_instrument(coin);
                ev.price = to_price_t(px);
                ev.quantity = to_qty_t(sz);
                // "B" = taker was buyer → buyer is taker → is_buyer_maker = false
                // "A" = taker was seller → buyer is maker → is_buyer_maker = true
                ev.is_buyer_maker = (side_str == "A");
                ev.side = (side_str == "B") ? Side::BUY : Side::SELL;
                ev.trade_id = tid;
            });
        } catch (const std::exception& e) {
            LOG_ERROR("Hyperliquid MD: Failed to parse trade: {}", e.what());
        }
    }
}

void HyperliquidMDConnector::on_bbo(const nlohmann::json& msg) {
    const auto data_ptr = msg.find("data");
    if (data_ptr == msg.end()) return;

    try {
        const auto& data = *data_ptr;
        const std::string coin = data.at("coin").get<std::string>();
        const uint64_t time_ms = data.value("time", uint64_t(0));

        // levels[0] = [[bid_price, bid_size]], levels[1] = [[ask_price, ask_size]]
        const auto& levels = data.at("levels");
        if (levels.size() < 2) return;

        const auto& bids = levels[0];
        const auto& asks = levels[1];
        if (bids.empty() || asks.empty()) return;

        const double bid_px = std::stod(bids[0][0].get<std::string>());
        const double bid_sz = std::stod(bids[0][1].get<std::string>());
        const double ask_px = std::stod(asks[0][0].get<std::string>());
        const double ask_sz = std::stod(asks[0][1].get<std::string>());

        EventSerializer::publish<QuoteEvent>(*event_buffer_, [&](QuoteEvent& ev) {
            ev.broker_id = BrokerId::HYPERLIQUID;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = time_ms * 1'000'000ULL;
            ev.set_instrument(coin);
            ev.bid_price = to_price_t(bid_px);
            ev.bid_quantity = to_qty_t(bid_sz);
            ev.ask_price = to_price_t(ask_px);
            ev.ask_quantity = to_qty_t(ask_sz);
        });
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: Failed to parse bbo: {}", e.what());
    }
}

void HyperliquidMDConnector::on_l2book(const nlohmann::json& msg) {
    const auto data_ptr = msg.find("data");
    if (data_ptr == msg.end()) return;

    try {
        const auto& data = *data_ptr;
        const std::string coin = data.at("coin").get<std::string>();
        const uint64_t time_ms = data.value("time", uint64_t(0));

        // levels[0] = bids [[price, size], ...], levels[1] = asks
        const auto& levels = data.at("levels");
        if (levels.size() < 2) return;

        const auto& bids = levels[0];
        const auto& asks = levels[1];
        const size_t total_levels = bids.size() + asks.size();

        if (total_levels == 0) return;

        const uint32_t event_size = static_cast<uint32_t>(
            sizeof(Level2UpdateEvent) + total_levels * sizeof(PriceLevel));

        EventSerializer::publish<Level2UpdateEvent>(*event_buffer_, event_size, [&](Level2UpdateEvent& ev) {
            ev.broker_id = BrokerId::HYPERLIQUID;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = time_ms * 1'000'000ULL;
            ev.set_instrument(coin);
            ev.is_snapshot = true;  // Hyperliquid sends full book on each update
            ev.sequence_num = time_ms;
            ev.num_levels = static_cast<uint16_t>(total_levels);

            size_t idx = 0;
            for (const auto& level : bids) {
                ev.levels[idx].price = to_price_t(std::stod(level[0].get<std::string>()));
                ev.levels[idx].quantity = to_qty_t(std::stod(level[1].get<std::string>()));
                ev.levels[idx].side = Side::BUY;
                ++idx;
            }
            for (const auto& level : asks) {
                ev.levels[idx].price = to_price_t(std::stod(level[0].get<std::string>()));
                ev.levels[idx].quantity = to_qty_t(std::stod(level[1].get<std::string>()));
                ev.levels[idx].side = Side::SELL;
                ++idx;
            }
        });
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: Failed to parse l2Book: {}", e.what());
    }
}

} // namespace slick::trader
