#include <slick-trader/brokers/hyperliquid/hyperliquid_md_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/utils/time.hpp>
#include <slick/net/http.hpp>
#include <hyperliquid/info.hpp>
#include <hyperliquid/utils/constants.hpp>
#include <hyperliquid/utils/l2_diff.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <cstring>

namespace slick::trader {

static constexpr uint32_t producer_id = static_cast<uint8_t>(BrokerId::HYPERLIQUID) * 2 - 1;

HyperliquidMDConnector::HyperliquidMDConnector(
    const json& config,
    slick::stream_buffer_multiplexer &mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& request_queue
)
    : enabled_(config.empty() ? false : config.value("enabled", true))
    , shm_name_(config.value("shm_name", ""))
    , control_queue_(control_queue)
    , request_queue_(request_queue)
    , request_cursor_(request_queue.initial_reading_index())
    , api_url_(config.value("api_url", hyperliquid::MAINNET_API_URL))
{
    auto& reg = BrokerStatusRegistry::instance();
    reg.set_name(BrokerId::HYPERLIQUID, "Hyperliquid");
    reg.set_md_enabled(BrokerId::HYPERLIQUID, enabled_);
    if (enabled_) {
        event_buffer_ = mux.add_producer(
            producer_id,
            config.value("queue_size", 1u << 26),
            config.value("control_size", 1u << 24),
            shm_name_.empty() ? nullptr : shm_name_.c_str()
        );
    } else {
        LOG_INFO("Hyperliquid MD connector is disabled");
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
            std::memcpy(event.shm_name, shm_name_.c_str(), sizeof(event.shm_name));
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
        info_->load_meta();
        connected_.store(true, std::memory_order_release);
        BrokerStatusRegistry::instance().set_md_connected(BrokerId::HYPERLIQUID, true);
        LOG_INFO("Hyperliquid MD: Connected");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: connect() failed: {}", e.what());
        info_.reset();
        connected_.store(false, std::memory_order_release);
        BrokerStatusRegistry::instance().set_md_connected(BrokerId::HYPERLIQUID, false);
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
        BrokerStatusRegistry::instance().set_md_connected(BrokerId::HYPERLIQUID, false);
        LOG_INFO("Hyperliquid MD: Disconnected");
    }
}

bool HyperliquidMDConnector::subscribe(const MDSubscription& request) {
    LOG_INFO("Hyperliquid MD: Subscribing to {} instruments (type: {})",
             request.num_instruments, static_cast<int>(request.subscription_type));

    // If already connected, subscribe immediately
    if (info_) {
        for (auto i = 0u; i < request.num_instruments; ++i) {
            const auto& instr = request.instruments[i];

            auto iter = info_->name_to_coin.find(instr);
            if (iter == info_->name_to_coin.end()) {
                LOG_ERROR("HYPERLIQUID skip subscription of an invalid coin {}", instr);
                continue;
            }

            coin_to_name_.emplace(std::make_pair(iter->second, instr));

            nlohmann::json sub;
            std::string ch_type;

            switch (request.subscription_type) {
                case SubscriptionType::TRADE:
                    trade_instruments_.push_back(instr);
                    ch_type = "trades";
                    sub = {{"type", "trades"}, {"coin", instr}};
                    break;
                case SubscriptionType::QUOTE:
                    quote_instruments_.push_back(instr);
                    ch_type = "bbo";
                    sub = {{"type", "bbo"}, {"coin", instr}};
                    break;
                case SubscriptionType::LEVEL2:
                    level2_instruments_.push_back(instr);
                    level2_depth_ = request.depth;
                    ch_type = "l2";
                    sub = {{"type", "l2"}, {"c", instr}};
                    break;
                case SubscriptionType::ORDER_BOOK:
                    level2_instruments_.push_back(instr);
                    level2_depth_ = request.depth;
                    ch_type = "l2Book";
                    sub = {{"type", "l2Book"}, {"coin", instr}, {"fast", true}};
                    break;
                case SubscriptionType::MBO:
                    LOG_WARN("Hyperliquid MD: no market-by-order feed, MBO subscription of {} ignored", instr);
                    continue;
            }

            int id = info_->subscribe(sub, [this, ch_type](const nlohmann::json& msg) {
                if (ch_type == "trades") on_trades(msg);
                else if (ch_type == "bbo") on_bbo(msg);
                else if (ch_type == "l2") on_l2(msg);
                else if (ch_type == "l2Book") on_l2book(msg);
                else LOG_WARN("Hyperliquid MD: Unknown channel type: {}", ch_type);
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
        case SubscriptionType::MBO:    break;
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
        if (info_) {
            info_->dispatch();
        }
        process_md_requests();
    }

    LOG_INFO("Hyperliquid MD: Worker thread stopped");
}

void HyperliquidMDConnector::process_md_requests() {
    auto [data, size] = request_queue_.read(request_cursor_);

    if (!data) {
        std::this_thread::yield();
        return;
    }

    const auto *event = reinterpret_cast<const Event*>(data);
    switch (event->type) {
        case EventType::SUBSCRIBE: {
            auto *sub = reinterpret_cast<const MDSubscription*>(event);
            if (sub->broker_id == BrokerId::HYPERLIQUID && enabled_) {
                subscribe(*sub);
            } else if (sub->broker_id == BrokerId::HYPERLIQUID) {
                LOG_WARN("Coinbase MD skip subscription, not enabled");
            }
            break;
        }
        case EventType::UNSUBSCRIBE: {
            auto *sub = reinterpret_cast<const MDSubscription*>(event);
            if (sub->broker_id == BrokerId::HYPERLIQUID && enabled_) {
                unsubscribe(*sub);
            }
            break;
        }
        default:
            LOG_ERROR("HYPERLIQUID MD: Unknown event type: {}", static_cast<int>(event->type));
            break;
    }
}

void HyperliquidMDConnector::subscribe_all() {
    auto subscribe_coin_list = [&](const std::vector<std::string>& coins,
                                   const std::string& ch_type) {
        for (const auto& coin : coins) {
            nlohmann::json sub = {{"type", ch_type}, {"coin", coin}};
            int id = info_->subscribe(sub, [this, ch_type](const nlohmann::json& msg) {
                if (ch_type == "trades") on_trades(msg);
                else if (ch_type == "bbo") on_bbo(msg);
                else if (ch_type == "l2") on_l2(msg);
                else if (ch_type == "l2Book") on_l2book(msg);
                else LOG_WARN("Hyperliquid MD: Unknown channel type: {}", ch_type);
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
            std::string instr = coin;
            auto iter = coin_to_name_.find(coin);
            if (iter != coin_to_name_.end()) {
                instr = iter->second;
            }

            EventSerializer::publish<TradeEvent>(*event_buffer_, [&](TradeEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.event_time_ns = time_ms * 1'000'000ULL;
                ev.set_instrument(instr);
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

        std::string instr = coin;
        auto iter = coin_to_name_.find(coin);
        if (iter != coin_to_name_.end()) {
            instr = iter->second;
        }

        EventSerializer::publish<QuoteEvent>(*event_buffer_, [&](QuoteEvent& ev) {
            ev.broker_id = BrokerId::HYPERLIQUID;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = time_ms * 1'000'000ULL;
            ev.set_instrument(instr);
            ev.bid_price = to_price_t(bid_px);
            ev.bid_quantity = to_qty_t(bid_sz);
            ev.ask_price = to_price_t(ask_px);
            ev.ask_quantity = to_qty_t(ask_sz);
        });
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: Failed to parse bbo: {}", e.what());
    }
}

void HyperliquidMDConnector::on_l2(const nlohmann::json& msg) {
    const auto data_ptr = msg.find("data");
    if (data_ptr == msg.end()) return;

    try {
        const auto& data = *data_ptr;
        const auto s_ptr = data.find("s");
        if (s_ptr != data.end() && s_ptr->is_string()) {
            // This is a snapshot response to a subscription request, not an actual l2 update

            const std::string coin = s_ptr->at("coin").get<std::string>();
            const uint64_t time_ms = s_ptr->value("t", uint64_t(0));

            std::string instr = coin;
            auto iter = coin_to_name_.find(coin);
            if (iter != coin_to_name_.end()) {
                instr = iter->second;
            }

            const auto& levels = s_ptr->at("levels");
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
                ev.set_instrument(instr);
                ev.is_snapshot = true;
                ev.sequence_num = time_ms;
                ev.num_levels = static_cast<uint16_t>(total_levels);

                size_t idx = 0;
                for (const auto& level : bids) {
                    ev.levels[idx].price = to_price_t(std::stod(level["px"].get<std::string>()));
                    ev.levels[idx].quantity = to_qty_t(std::stod(level["sz"].get<std::string>()));
                    ev.levels[idx].side = Side::BUY;

                    bid_prices_.insert(ev.levels[idx].price); // Add to bids set
                    ++idx;
                }
                for (const auto& level : asks) {
                    ev.levels[idx].price = to_price_t(std::stod(level["px"].get<std::string>()));
                    ev.levels[idx].quantity = to_qty_t(std::stod(level["sz"].get<std::string>()));
                    ev.levels[idx].side = Side::SELL;
                    
                    ask_prices_.insert(ev.levels[idx].price); // Add to asks set
                    ++idx;
                }
            });
            return;
        }

        const auto c_ptr = data.find("c");
        if (c_ptr == data.end() || !c_ptr->is_string()) return;

        const auto diff = hyperliquid::decode_l2_diff(c_ptr->get<std::string>());

        const std::string coin = diff.at("c").get<std::string>();
        const uint64_t time_ms = diff.value("t", uint64_t(0));

        std::string instr = coin;
        auto iter = coin_to_name_.find(coin);
        if (iter != coin_to_name_.end()) {
            instr = iter->second;
        }

        LOG_TRACE("Hyperliquid MD: l2 for {}: {}", instr, c_ptr->get<std::string>());
        LOG_TRACE("Hyperliquid MD: l2 diff for {}: {}", instr, diff.dump());

        const auto& removed_levels = diff.at("r");
        if (!removed_levels.empty() && removed_levels.is_array() && removed_levels.size() == 2) {
            const auto& bids = removed_levels[0];
            const auto& asks = removed_levels[1];
            const size_t total_levels = bids.size() + asks.size();
            if (total_levels) {
                // LOG_TRACE(diff.at("r").dump());

                const uint32_t event_size = static_cast<uint32_t>(
                    sizeof(Level2UpdateEvent) + total_levels * sizeof(PriceLevel));

                EventSerializer::publish<Level2UpdateEvent>(*event_buffer_, event_size, [&](Level2UpdateEvent& ev) {
                    ev.broker_id = BrokerId::HYPERLIQUID;
                    ev.timestamp_ns = get_timestamp_ns();
                    ev.event_time_ns = time_ms * 1'000'000ULL;
                    ev.set_instrument(instr);
                    ev.is_snapshot = false;
                    ev.sequence_num = time_ms;

                    size_t idx = 0;
                    // The bid side index is in descending order, so we can iterate through it directly
                    for (const auto& level_index : bids) {
                        auto index = level_index.get<int>();
                        if (index < 0 || index >= static_cast<int>(bid_prices_.size())) {
                            continue;
                        }

                        auto price = *std::next(bid_prices_.begin(), index);
                        // LOG_TRACE("Removing bid level at index {}: price={}", index, price);
                        bid_prices_.erase(price); // Remove the price from the set

                        ev.levels[idx].price = price;
                        ev.levels[idx].quantity = 0; // Level removed, quantity is zero
                        ev.levels[idx].side = Side::BUY;
                        ++idx;
                    }

                    // Remove levels from the ask side in reverse order to avoid invalidating indices
                    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
                        auto index = it->get<int>();
                        if (index < 0 || index >= static_cast<int>(ask_prices_.size())) {
                            continue;
                        }

                        auto price = *std::next(ask_prices_.begin(), index);
                        // LOG_TRACE("Removing ask level at index {}: price={}", index, price);
                        ask_prices_.erase(price); // Remove the price from the set

                        ev.levels[idx].price = price;
                        ev.levels[idx].quantity = 0; // Level removed, quantity is zero
                        ev.levels[idx].side = Side::SELL;
                        ++idx;
                    }

                    ev.num_levels = static_cast<uint16_t>(idx);
                });
            }
        }

        const auto& levels = diff.at("l");
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
            ev.set_instrument(instr);
            ev.is_snapshot = false;
            ev.sequence_num = time_ms;
            ev.num_levels = static_cast<uint16_t>(total_levels);

            size_t idx = 0;
            for (const auto& level : bids) {
                ev.levels[idx].price = to_price_t(std::stod(level["p"].get<std::string>()));
                ev.levels[idx].quantity = to_qty_t(std::stod(level["s"].get<std::string>()));
                ev.levels[idx].side = Side::BUY;

                bid_prices_.insert(ev.levels[idx].price); // Add to bids set
                ++idx;
            }
            for (const auto& level : asks) {
                ev.levels[idx].price = to_price_t(std::stod(level["p"].get<std::string>()));
                ev.levels[idx].quantity = to_qty_t(std::stod(level["s"].get<std::string>()));
                ev.levels[idx].side = Side::SELL;
                
                ask_prices_.insert(ev.levels[idx].price); // Add to asks set
                ++idx;
            }
        });
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: Failed to parse l2 diff: {}", e.what());
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

        std::string instr = coin;
        auto iter = coin_to_name_.find(coin);
        if (iter != coin_to_name_.end()) {
            instr = iter->second;
        }

        const uint32_t event_size = static_cast<uint32_t>(
            sizeof(Level2UpdateEvent) + total_levels * sizeof(PriceLevel));

        EventSerializer::publish<Level2UpdateEvent>(*event_buffer_, event_size, [&](Level2UpdateEvent& ev) {
            ev.broker_id = BrokerId::HYPERLIQUID;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = time_ms * 1'000'000ULL;
            ev.set_instrument(instr);
            ev.is_snapshot = true;  // Hyperliquid sends full book on each update
            ev.sequence_num = time_ms;
            ev.num_levels = static_cast<uint16_t>(total_levels);

            size_t idx = 0;
            for (const auto& level : bids) {
                ev.levels[idx].price = to_price_t(std::stod(level["px"].get<std::string>()));
                ev.levels[idx].quantity = to_qty_t(std::stod(level["sz"].get<std::string>()));
                ev.levels[idx].num_orders = level["n"].get<uint32_t>();
                ev.levels[idx].side = Side::BUY;
                ++idx;
            }
            for (const auto& level : asks) {
                ev.levels[idx].price = to_price_t(std::stod(level["px"].get<std::string>()));
                ev.levels[idx].quantity = to_qty_t(std::stod(level["sz"].get<std::string>()));
                ev.levels[idx].num_orders = level["n"].get<uint32_t>();
                ev.levels[idx].side = Side::SELL;
                ++idx;
            }
        });
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid MD: Failed to parse l2Book: {}", e.what());
    }
}

} // namespace slick::trader
