#include <slick-trader/brokers/hyperliquid/hyperliquid_order_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/utils/time.hpp>
#include <hyperliquid/info.hpp>
#include <hyperliquid/exchange.hpp>
#include <hyperliquid/utils/types.hpp>
#include <hyperliquid/utils/constants.hpp>
#include <slick/logger.hpp>
#include <cassert>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace slick::trader {

// ── Helpers ───────────────────────────────────────────────────────────────────

static hyperliquid::Tif to_hl_tif(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::IOC:
        case TimeInForce::FOK: return hyperliquid::Tif::Ioc;
        default:               return hyperliquid::Tif::Gtc;
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

HyperliquidOrderConnector::HyperliquidOrderConnector(
    const json& config,
    slick::stream_buffer_multiplexer &mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& request_queue
)
    : enabled_(config.empty() ? false : config.value("enabled", true))
    , shm_name_(config.value("callback_shm_name", ""))
    , event_buffer_(mux.add_producer(
        mux.producer_count(),
        config.value("queue_size", 1u << 26),
        config.value("control_size", 1u << 24),
        shm_name_.empty() ? nullptr : shm_name_.c_str()))
    , control_queue_(control_queue)
    , request_queue_(request_queue)
    , request_cursor_(request_queue.initial_reading_index())
    , api_url_(config.value("api_url", hyperliquid::MAINNET_API_URL))
{
    BrokerStatusRegistry::instance().set_order_enabled(BrokerId::HYPERLIQUID, enabled_);
    if (!enabled_) {
        LOG_INFO("Hyperliquid order connector is disabled");
    }
}

HyperliquidOrderConnector::~HyperliquidOrderConnector() {
    stop();
}

void HyperliquidOrderConnector::register_order_connector() {
    if (enabled_) {
        EventSerializer::publish<OrderConnectorRegisterEvent>(control_queue_, [=](OrderConnectorRegisterEvent& event) {
            event.broker_id = BrokerId::COINBASE;
            event.producer_id = event_buffer_->producer_id();
            event.fee_structure.type = FeeType::PERCENTAGE;
            event.fee_structure.rate = 0.005;
        });
    }
}

bool HyperliquidOrderConnector::connect() {
    LOG_INFO("Hyperliquid Order: Connecting to {}", api_url_);

    if (private_key_.empty()) {
        LOG_ERROR("Hyperliquid Order: Private key not set (set HYPERLIQUID_PRIVATE_KEY env var)");
        return false;
    }

    try {
        info_ = std::make_shared<hyperliquid::Info>(api_url_);
        exchange_ = std::make_unique<hyperliquid::Exchange>(private_key_, api_url_, info_);
        wallet_address_ = exchange_->wallet_address();
        LOG_INFO("Hyperliquid Order: Wallet address: {}", wallet_address_);

        // Subscribe to user fill stream
        info_->subscribe(
            {{"type", "userFills"}, {"user", wallet_address_}},
            [this](const nlohmann::json& msg) {
                const auto data = msg.find("data");
                if (data == msg.end()) return;
                const auto fills = data->find("fills");
                if (fills == data->end() || !fills->is_array()) return;
                for (const auto& fill : *fills) {
                    on_user_fill(fill);
                }
            });

        // Subscribe to order status stream
        info_->subscribe(
            {{"type", "orderUpdates"}, {"user", wallet_address_}},
            [this](const nlohmann::json& msg) {
                const auto data = msg.find("data");
                if (data == msg.end() || !data->is_array()) return;
                for (const auto& update : *data) {
                    on_order_update(update);
                }
            });

        connected_.store(true, std::memory_order_release);
        BrokerStatusRegistry::instance().set_order_connected(BrokerId::HYPERLIQUID, true);
        LOG_INFO("Hyperliquid Order: Connected");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid Order: connect() failed: {}", e.what());
        exchange_.reset();
        info_.reset();
        connected_.store(false, std::memory_order_release);
        BrokerStatusRegistry::instance().set_order_connected(BrokerId::HYPERLIQUID, false);
        return false;
    }
}

void HyperliquidOrderConnector::disconnect() {
    if (info_ || exchange_) {
        LOG_INFO("Hyperliquid Order: Disconnecting");
        exchange_.reset();
        info_.reset();
        connected_.store(false, std::memory_order_release);
        BrokerStatusRegistry::instance().set_order_connected(BrokerId::HYPERLIQUID, false);
        LOG_INFO("Hyperliquid Order: Disconnected");
    }
}

void HyperliquidOrderConnector::start() {
    if (running_.load(std::memory_order_relaxed)) {
        LOG_WARN("Hyperliquid Order: Already running");
        return;
    }

    if (!enabled_) {
        return;
    }

    LOG_INFO("Hyperliquid Order: Starting");
    
    if (connect()) {
        worker_thread_ = std::thread(&HyperliquidOrderConnector::run, this);
        running_.store(true, std::memory_order_release);
        LOG_INFO("Hyperliquid Order: Started");
    }
}

void HyperliquidOrderConnector::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    LOG_INFO("Hyperliquid Order: Stopping");
    disconnect();
    running_.store(false, std::memory_order_release);

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    LOG_INFO("Hyperliquid Order: Stopped");
}

void HyperliquidOrderConnector::run() {
    LOG_INFO("Hyperliquid Order: Worker thread started");

    while (running_.load(std::memory_order_acquire)) {
        process_order_requests();
    }

    LOG_INFO("Hyperliquid Order: Worker thread stopped");
}

// ── Order request dispatch ────────────────────────────────────────────────────

void HyperliquidOrderConnector::process_order_requests() {

    auto [data, size] = request_queue_.read(request_cursor_);
    if (!data) {
        std::this_thread::yield();
        return;
    }

    const auto* event = reinterpret_cast<const Event*>(data);
    switch (event->type) {
        case EventType::NEW_ORDER:
            handle_new_order(*reinterpret_cast<const NewOrderEvent*>(data));
            break;
        case EventType::MODIFY_ORDER:
            handle_modify_order(*reinterpret_cast<const ModifyOrderEvent*>(data));
            break;
        case EventType::CANCEL_ORDER:
            handle_cancel_order(*reinterpret_cast<const CancelOrderEvent*>(data));
            break;
        default:
            LOG_ERROR("Hyperliquid Order: Unknown event type: {}", static_cast<int>(event->type));
            break;
    }
}

// ── New order ─────────────────────────────────────────────────────────────────

void HyperliquidOrderConnector::handle_new_order(const NewOrderEvent& req) {
    if (!exchange_) {
        publish_reject(req, "Not connected");
        return;
    }

    std::string coin{req.instrument};
    bool is_buy = (req.side == Side::BUY);
    double sz = from_qty_t(req.quantity);

    // Assign a Cloid from our sequential counter
    uint64_t counter = cloid_counter_.fetch_add(1, std::memory_order_relaxed);
    auto cloid = hyperliquid::Cloid::from_int(counter);
    std::string cloid_raw{cloid.to_raw()};

    {
        std::lock_guard<std::mutex> lock(order_map_mutex_);
        cloid_to_client_id_[cloid_raw] = std::string{req.client_order_id};
    }

    LOG_INFO("Hyperliquid Order: New order coin={} side={} sz={} px={} cloid={}",
             coin, is_buy ? "BUY" : "SELL", sz, from_price_t(req.price), cloid_raw);

    try {
        nlohmann::json response;

        if (req.order_type == OrderType::MARKET) {
            response = exchange_->market_open(coin, is_buy, sz, 0.05, cloid);
        } else if (req.order_type == OrderType::STOP || req.order_type == OrderType::STOP_LIMIT) {
            double trigger_px = (req.stop_price != NULL_PRICE) ? from_price_t(req.stop_price) : from_price_t(req.price);
            bool is_market_trigger = (req.order_type == OrderType::STOP);
            double limit_px = (req.price != NULL_PRICE) ? from_price_t(req.price) : trigger_px;
            response = exchange_->order(coin, is_buy, sz, limit_px,
                hyperliquid::TriggerOrderType{trigger_px, is_market_trigger, "sl"},
                false, cloid);
        } else {
            // LIMIT or BRACKET
            double limit_px = from_price_t(req.price);
            hyperliquid::Tif tif = req.post_only ? hyperliquid::Tif::Alo : to_hl_tif(req.time_in_force);
            response = exchange_->order(coin, is_buy, sz, limit_px,
                hyperliquid::LimitOrderType{tif}, false, cloid);
        }

        if (response.value("status", "") != "ok") {
            std::string err = response.dump();
            LOG_ERROR("Hyperliquid Order: Order rejected by exchange: {}", err);
            publish_reject(req, err);
            return;
        }

        const auto& statuses = response["response"]["data"]["statuses"];
        if (statuses.empty()) {
            publish_reject(req, "Empty status in response");
            return;
        }

        const auto& status = statuses[0];

        if (status.contains("error")) {
            std::string err = status["error"].get<std::string>();
            LOG_ERROR("Hyperliquid Order: Order error: {}", err);
            publish_reject(req, err);
            return;
        }

        int64_t oid = 0;

        if (status.contains("resting")) {
            oid = status["resting"]["oid"].get<int64_t>();
            std::string oid_str = std::to_string(oid);

            {
                std::lock_guard<std::mutex> lock(order_map_mutex_);
                oid_to_client_id_[oid] = std::string{req.client_order_id};
                oid_to_instrument_[oid] = coin;
            }

            LOG_INFO("Hyperliquid Order: Order resting oid={} client_order_id={}", oid, req.client_order_id);

            EventSerializer::publish<OrderAckEvent>(*event_buffer_, [&](OrderAckEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(req.client_order_id);
                ev.set_order_id(oid_str);
                ev.side = req.side;
                ev.order_type = req.order_type;
                ev.price = req.price;
                ev.stop_price = req.stop_price;
                ev.quantity = req.quantity;
                ev.time_in_force = req.time_in_force;
                ev.post_only = req.post_only;
                ev.status = OrderStatus::ACKNOWLEDGED;
            });

        } else if (status.contains("filled")) {
            oid = status["filled"]["oid"].get<int64_t>();
            double avg_px = std::stod(status["filled"]["avgPx"].get<std::string>());
            double total_sz = std::stod(status["filled"]["totalSz"].get<std::string>());
            std::string oid_str = std::to_string(oid);

            {
                std::lock_guard<std::mutex> lock(order_map_mutex_);
                oid_to_client_id_[oid] = std::string{req.client_order_id};
                oid_to_instrument_[oid] = coin;
            }

            LOG_INFO("Hyperliquid Order: Order filled oid={} avgPx={} totalSz={}", oid, avg_px, total_sz);

            EventSerializer::publish<OrderAckEvent>(*event_buffer_, [&](OrderAckEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(req.client_order_id);
                ev.set_order_id(oid_str);
                ev.side = req.side;
                ev.order_type = req.order_type;
                ev.price = to_price_t(avg_px);
                ev.stop_price = req.stop_price;
                ev.quantity = req.quantity;
                ev.time_in_force = req.time_in_force;
                ev.post_only = req.post_only;
                ev.status = OrderStatus::FILLED;
            });

            EventSerializer::publish<OrderFillEvent>(*event_buffer_, [&](OrderFillEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(req.client_order_id);
                ev.set_order_id(oid_str);
                ev.side = req.side;
                ev.fill_price = to_price_t(avg_px);
                ev.fill_quantity = to_qty_t(total_sz);
                ev.cumulative_filled = to_qty_t(total_sz);
                ev.remaining_quantity = 0;
                ev.fee_amount = 0;  // will be updated by userFills WebSocket
            });
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid Order: Exception in handle_new_order: {}", e.what());
        publish_reject(req, e.what());
    }
}

// ── Modify order ──────────────────────────────────────────────────────────────

void HyperliquidOrderConnector::handle_modify_order(const ModifyOrderEvent& req) {
    if (!exchange_) {
        LOG_ERROR("Hyperliquid Order: handle_modify_order called while not connected");
        return;
    }

    int64_t oid = 0;
    try {
        oid = std::stoll(std::string{req.order_id});
    } catch (...) {
        LOG_ERROR("Hyperliquid Order: Invalid order_id in modify request: {}", req.order_id);
        return;
    }

    std::string coin{req.instrument};
    LOG_INFO("Hyperliquid Order: Modify oid={} new_px={} new_qty={}",
             oid, from_price_t(req.new_price), from_qty_t(req.new_quantity));

    try {
        hyperliquid::Tif tif = req.new_post_only ? hyperliquid::Tif::Alo : to_hl_tif(req.new_time_in_force);
        hyperliquid::OrderRequest new_order{
            .coin = coin,
            .is_buy = (req.side == Side::BUY),
            .sz = from_qty_t(req.new_quantity),
            .limit_px = from_price_t(req.new_price),
            .order_type = hyperliquid::LimitOrderType{tif},
            .reduce_only = false,
        };

        auto response = exchange_->modify_order(oid, new_order);

        if (response.value("status", "") == "ok") {
            LOG_INFO("Hyperliquid Order: Modify accepted oid={}", oid);
            EventSerializer::publish<OrderAckEvent>(*event_buffer_, [&](OrderAckEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(req.client_order_id);
                ev.set_order_id(req.order_id);
                ev.side = req.side;
                ev.price = req.new_price;
                ev.stop_price = req.new_stop_price;
                ev.quantity = req.new_quantity;
                ev.time_in_force = req.new_time_in_force;
                ev.post_only = req.new_post_only;
                ev.status = OrderStatus::ACKNOWLEDGED;
            });
        } else {
            std::string err = response.dump();
            LOG_ERROR("Hyperliquid Order: Modify failed oid={}: {}", oid, err);
            EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&](OrderRejectEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(req.client_order_id);
                ev.set_order_id(req.order_id);
                ev.set_reason(err.substr(0, 127));
                ev.side = req.side;
                ev.price = req.new_price;
                ev.quantity = req.new_quantity;
            });
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid Order: Exception in handle_modify_order: {}", e.what());
    }
}

// ── Cancel order ──────────────────────────────────────────────────────────────

void HyperliquidOrderConnector::handle_cancel_order(const CancelOrderEvent& req) {
    if (!exchange_) {
        LOG_ERROR("Hyperliquid Order: handle_cancel_order called while not connected");
        return;
    }

    int64_t oid = 0;
    try {
        oid = std::stoll(std::string{req.order_id});
    } catch (...) {
        LOG_ERROR("Hyperliquid Order: Invalid order_id in cancel request: {}", req.order_id);
        return;
    }

    std::string coin{req.instrument};
    LOG_INFO("Hyperliquid Order: Cancel oid={}", oid);

    try {
        auto response = exchange_->cancel(coin, oid);

        if (response.value("status", "") == "ok") {
            LOG_INFO("Hyperliquid Order: Cancel accepted oid={}", oid);
            // Cancel events also arrive via orderUpdates WebSocket; emit here too for reliability
            EventSerializer::publish<OrderCancelEvent>(*event_buffer_, [&](OrderCancelEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(req.client_order_id);
                ev.set_order_id(req.order_id);
                ev.set_reason("CANCELLED");
                ev.remaining_quantity = 0;
            });
        } else {
            LOG_ERROR("Hyperliquid Order: Cancel failed oid={}: {}", oid, response.dump());
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid Order: Exception in handle_cancel_order: {}", e.what());
    }
}

// ── WebSocket callbacks (invoked from WebsocketManager thread) ────────────────

void HyperliquidOrderConnector::on_user_fill(const nlohmann::json& fill) {
    try {
        int64_t oid = fill.at("oid").get<int64_t>();
        uint64_t tid = fill.value("tid", uint64_t(0));

        // Deduplicate fills we already handled via REST response
        {
            std::lock_guard<std::mutex> lock(order_map_mutex_);
            if (!seen_fill_tids_.insert(tid).second) return;  // already seen
        }

        std::string client_order_id;
        std::string instrument;
        {
            std::lock_guard<std::mutex> lock(order_map_mutex_);
            auto cit = oid_to_client_id_.find(oid);
            if (cit == oid_to_client_id_.end()) return;  // unknown order
            client_order_id = cit->second;
            auto iit = oid_to_instrument_.find(oid);
            instrument = (iit != oid_to_instrument_.end()) ? iit->second : fill.value("coin", "");
        }

        const double px = std::stod(fill.at("px").get<std::string>());
        const double sz = std::stod(fill.at("sz").get<std::string>());
        const std::string side_str = fill.value("side", "B");
        const double fee_raw = std::stod(fill.value("fee", "0"));  // negative = fee paid
        const uint64_t time_ms = fill.value("time", uint64_t(0));
        const std::string oid_str = std::to_string(oid);
        const std::string tid_str = std::to_string(tid);

        EventSerializer::publish<OrderFillEvent>(*event_buffer_, [&](OrderFillEvent& ev) {
            ev.broker_id = BrokerId::HYPERLIQUID;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = time_ms * 1'000'000ULL;
            ev.set_instrument(instrument);
            ev.set_client_order_id(client_order_id);
            ev.set_order_id(oid_str);
            ev.set_fill_id(tid_str);
            ev.side = (side_str == "B") ? Side::BUY : Side::SELL;
            ev.fill_price = to_price_t(px);
            ev.fill_quantity = to_qty_t(sz);
            ev.cumulative_filled = to_qty_t(sz);  // cumulative not available per-fill
            ev.remaining_quantity = 0;
            ev.fee_amount = to_price_t(std::abs(fee_raw));
        });

        LOG_DEBUG("Hyperliquid Order: Fill oid={} px={} sz={} fee={}", oid, px, sz, fee_raw);

    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid Order: Failed to process user fill: {}", e.what());
    }
}

void HyperliquidOrderConnector::on_order_update(const nlohmann::json& update) {
    try {
        std::string status = update.value("status", "");
        if (status != "canceled" && status != "rejected") return;

        const auto order_ptr = update.find("order");
        if (order_ptr == update.end()) return;

        const auto& order = *order_ptr;
        int64_t oid = order.value("oid", int64_t(0));
        std::string coin = order.value("coin", "");

        std::string client_order_id;
        {
            std::lock_guard<std::mutex> lock(order_map_mutex_);
            auto it = oid_to_client_id_.find(oid);
            if (it == oid_to_client_id_.end()) return;
            client_order_id = it->second;
        }

        std::string oid_str = std::to_string(oid);
        double remaining_sz = std::stod(order.value("sz", "0"));

        if (status == "canceled") {
            LOG_INFO("Hyperliquid Order: Order canceled via WebSocket oid={}", oid);
            EventSerializer::publish<OrderCancelEvent>(*event_buffer_, [&](OrderCancelEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(client_order_id);
                ev.set_order_id(oid_str);
                ev.set_reason("CANCELLED");
                ev.remaining_quantity = to_qty_t(remaining_sz);
            });
        } else if (status == "rejected") {
            LOG_WARN("Hyperliquid Order: Order rejected via WebSocket oid={}", oid);
            EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&](OrderRejectEvent& ev) {
                ev.broker_id = BrokerId::HYPERLIQUID;
                ev.timestamp_ns = get_timestamp_ns();
                ev.set_instrument(coin);
                ev.set_client_order_id(client_order_id);
                ev.set_order_id(oid_str);
                ev.set_reason("REJECTED");
            });
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Hyperliquid Order: Failed to process order update: {}", e.what());
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void HyperliquidOrderConnector::publish_reject(const NewOrderEvent& req, const std::string& reason) {
    EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&](OrderRejectEvent& ev) {
        ev.broker_id = BrokerId::HYPERLIQUID;
        ev.timestamp_ns = get_timestamp_ns();
        ev.set_instrument(req.instrument);
        ev.set_client_order_id(req.client_order_id);
        ev.set_reason(reason.substr(0, 127));
        ev.side = req.side;
        ev.price = req.price;
        ev.stop_price = req.stop_price;
        ev.quantity = req.quantity;
    });
}

} // namespace slick::trader
