#pragma once

#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/instrument_definition_event.hpp>
#include <slick-trader/events/instrument_status_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/mbo_update_event.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/utils/time.hpp>
#include <slick/logger.hpp>
#include <slick/queue.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace slick::trader {

// A broker's producer ids in the event multiplexer: odd for its market data, even for
// its order events.
constexpr uint32_t md_producer_id(BrokerId broker) noexcept { return static_cast<uint32_t>(broker) * 2 - 1; }
constexpr uint32_t order_producer_id(BrokerId broker) noexcept { return static_cast<uint32_t>(broker) * 2; }

// Copies `name` into a register event's fixed shm_name field.
template <typename E>
void set_shm_name(E& event, const std::string& name) {
    const size_t n = std::min(name.size(), sizeof(event.shm_name) - 1);
    std::memcpy(event.shm_name, name.data(), n);
    event.shm_name[n] = '\0';
}

// Calls `f(offset, n)` for `count` items in runs of at most `max`, and once with none
// when there are none. An empty snapshot is still published: it is what clears a
// subscriber's book.
template <typename F>
void for_each_part(size_t count, size_t max, F&& f) {
    size_t offset = 0;
    do {
        const size_t n = std::min(count - offset, max);
        f(offset, n);
        offset += n;
    } while (offset < count);
}

// One instrument as a market data connector describes it.
struct MdDefinition {
    std::string_view symbol;
    price_t tick_size = 0;
    double multiplier = 1.0;
    qty_t min_quantity = 0;
    qty_t max_quantity = 0;
    qty_t quantity_increment = 0;
    std::string_view currency;
};

// Publishes `d` into `buffer` as `broker`'s InstrumentDefinitionEvent. From the buffer's one writer.
inline bool publish_definition(slick::stream_buffer_multiplexer::producer_buffer& buffer, BrokerId broker,
                               const MdDefinition& d) {
    return EventSerializer::publish<InstrumentDefinitionEvent>(buffer, [&](InstrumentDefinitionEvent& ev) {
        ev.broker_id = broker;
        ev.timestamp_ns = get_timestamp_ns();
        ev.set_instrument(d.symbol);
        ev.tick_size = d.tick_size;
        ev.contract_multiplier = d.multiplier;
        ev.min_quantity = d.min_quantity;
        ev.max_quantity = d.max_quantity;
        ev.quantity_increment = d.quantity_increment;
        ev.set_currency(d.currency);
    });
}

// A market data connector's output: its producer buffer in the event multiplexer, and
// the events written into it. Single-threaded: the connector's worker is the one writer.
class MdEventWriter {
public:
    // A Level2UpdateEvent carries a uint16 level count; a book is published in events of
    // at most this many levels.
    static constexpr size_t MAX_LEVELS_PER_EVENT = 1024;
    // And an MboUpdateEvent this many orders, which makes the two records about the same size.
    static constexpr size_t MAX_ORDERS_PER_EVENT = 640;

    explicit MdEventWriter(BrokerId broker) noexcept : broker_(broker) {}

    // Adds the producer, sized by `queue_size` / `control_size` and named by `shm_name`
    // in `config` when it sets them.
    void open(slick::stream_buffer_multiplexer& mux, const nlohmann::json& config) {
        shm_name_ = config.value("shm_name", std::string());
        buffer_ = mux.add_producer(md_producer_id(broker_), config.value("queue_size", 1u << 26),
                                   config.value("control_size", 1u << 24),
                                   shm_name_.empty() ? nullptr : shm_name_.c_str());
    }
    [[nodiscard]] bool is_open() const noexcept { return buffer_ != nullptr; }

    void register_connector(slick::queue<uint8_t>& control_queue) const {
        EventSerializer::publish<RegisterEvent>(control_queue, [this](RegisterEvent& event) {
            event.broker_id = broker_;
            event.component_type = ComponentType::MARKET_DATA;
            event.producer_id = md_producer_id(broker_);
            set_shm_name(event, shm_name_);
        });
    }

    void trade(std::string_view symbol, price_t price, qty_t qty, Side aggressor, uint64_t event_time_ns,
               uint64_t trade_id = 0) {
        EventSerializer::publish<TradeEvent>(*buffer_, [&](TradeEvent& ev) {
            ev.broker_id = broker_;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = event_time_ns;
            ev.set_instrument(symbol);
            ev.price = price;
            ev.quantity = qty;
            ev.side = aggressor;
            // A sell aggressor hit a resting buyer: the buyer was the maker.
            ev.is_buyer_maker = aggressor == Side::SELL;
            ev.trade_id = trade_id;
        });
    }

    // Changed levels (quantity 0 = the level emptied), or with `is_snapshot` every level.
    void levels(std::string_view symbol, std::span<const PriceLevel> levels, bool is_snapshot,
                uint64_t event_time_ns, uint64_t sequence) {
        for_each_part(levels.size(), MAX_LEVELS_PER_EVENT, [&](size_t offset, size_t n) {
            const auto event_size = static_cast<uint32_t>(sizeof(Level2UpdateEvent) + n * sizeof(PriceLevel));
            EventSerializer::publish<Level2UpdateEvent>(*buffer_, event_size, [&](Level2UpdateEvent& ev) {
                ev.broker_id = broker_;
                ev.timestamp_ns = get_timestamp_ns();
                ev.event_time_ns = event_time_ns;
                ev.set_instrument(symbol);
                // Only the first part of a snapshot clears the book.
                ev.is_snapshot = is_snapshot && offset == 0;
                ev.sequence_num = sequence;
                ev.num_levels = static_cast<uint16_t>(n);
                if (n > 0) {
                    std::memcpy(ev.levels, levels.data() + offset, n * sizeof(PriceLevel));
                }
            });
        });
    }

    // One exchange event's order changes, or with `is_snapshot` every resting order.
    void orders(std::string_view symbol, std::span<const MboOrder> orders, bool is_snapshot,
                uint64_t event_time_ns, uint64_t sequence) {
        for_each_part(orders.size(), MAX_ORDERS_PER_EVENT, [&](size_t offset, size_t n) {
            EventSerializer::publish<MboUpdateEvent>(*buffer_, MboUpdateEvent::size_for(n), [&](MboUpdateEvent& ev) {
                ev.broker_id = broker_;
                ev.timestamp_ns = get_timestamp_ns();
                ev.event_time_ns = event_time_ns;
                ev.set_instrument(symbol);
                // Only the first part of a snapshot clears the book, and only after the
                // last is the book whole.
                ev.is_snapshot = is_snapshot && offset == 0;
                ev.is_last = offset + n == orders.size();
                ev.sequence_num = sequence;
                ev.num_orders = static_cast<uint16_t>(n);
                if (n > 0) {
                    std::memcpy(ev.orders, orders.data() + offset, n * sizeof(MboOrder));
                }
            });
        });
    }

    void status(std::string_view symbol, TradingStatus status, bool cancel_allowed, std::string_view reason,
                uint64_t event_time_ns) {
        EventSerializer::publish<InstrumentStatusEvent>(*buffer_, [&](InstrumentStatusEvent& ev) {
            ev.broker_id = broker_;
            ev.timestamp_ns = get_timestamp_ns();
            ev.event_time_ns = event_time_ns;
            ev.set_instrument(symbol);
            ev.status = status;
            ev.cancel_allowed = cancel_allowed;
            ev.set_reason(reason);
        });
    }

    using Definition = MdDefinition;

    void definition(const Definition& d) { publish_definition(*buffer_, broker_, d); }

    [[nodiscard]] BrokerId broker() const noexcept { return broker_; }

private:
    BrokerId broker_;
    std::string shm_name_;
    std::shared_ptr<slick::stream_buffer_multiplexer::producer_buffer> buffer_;
};

// Reads one request off `broker`'s market data request queue (MdRequestQueues) and hands a
// subscribe to `on_subscribe(const MDSubscription&)` and an unsubscribe to `on_unsubscribe`.
// Returns whether a request was read.
//
// The handlers get the request where it lies in the queue, valid until they return: keep what
// outlives the call. It is reported read to BrokerStatusRegistry after they return, and
// StrategyManager writes nothing over what this connector has not reported read. A connector
// reading the queue without this helper reports the same way, or its requests wait on it.
template <typename Subscribe, typename Unsubscribe>
bool process_md_request(slick::queue<uint8_t>& queue, uint64_t& cursor, BrokerId broker, Subscribe&& on_subscribe,
                        Unsubscribe&& on_unsubscribe) {
    auto [data, size] = queue.read(cursor);
    if (!data) {
        return false;
    }
    const auto& request = *reinterpret_cast<const MDSubscription*>(data);
    // Only a connector that is not waited on, with market data off, can be lapped: what it read
    // then is dropped, not read past its end.
    if (size >= sizeof(MDSubscription) && request.broker_id == broker &&
        size == sizeof(MDSubscription) + uint64_t{request.num_instruments} * sizeof(request.instruments[0])) {
        switch (request.type) {
        case EventType::SUBSCRIBE:
            on_subscribe(request);
            break;
        case EventType::UNSUBSCRIBE:
            on_unsubscribe(request);
            break;
        default:
            LOG_ERROR("{} market data request dropped: unknown type {}", to_string(broker),
                      static_cast<int>(request.type));
            break;
        }
    } else {
        LOG_WARN("{} market data request dropped: {} bytes that read as no request of its own, as when "
                 "lapped while not waited on",
                 to_string(broker), size);
    }
    BrokerStatusRegistry::instance().set_md_request_cursor(broker, cursor);
    return true;
}

} // namespace slick::trader
