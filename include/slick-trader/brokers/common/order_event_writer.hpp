#pragma once

#include <slick-trader/brokers/common/fill_journal.hpp>
#include <slick-trader/brokers/common/md_event_writer.hpp>
#include <slick-trader/brokers/iorder_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/utils/fee_structure.hpp>
#include <slick/logger.hpp>
#include <slick/queue.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace slick::trader {

// An order connector's output: its producer buffer in the event multiplexer, and the
// order events written into it. Single-threaded: the connector's worker is the one writer.
class OrderEventWriter {
public:
    explicit OrderEventWriter(BrokerId broker) noexcept : broker_(broker) {}

    // Adds the producer, sized by `queue_size` / `control_size` and named by `shm_name`
    // in `config` when it sets them.
    void open(slick::stream_buffer_multiplexer& mux, const nlohmann::json& config) {
        shm_name_ = config.value("shm_name", std::string());
        buffer_ = mux.add_producer(order_producer_id(broker_), config.value("queue_size", 1u << 24),
                                   config.value("control_size", 1u << 18),
                                   shm_name_.empty() ? nullptr : shm_name_.c_str());
    }
    [[nodiscard]] bool is_open() const noexcept { return buffer_ != nullptr; }

    // `fees` is the fallback for a fill that carries no fee of its own.
    void register_connector(slick::queue<uint8_t>& control_queue, const FeeStructure& fees) const {
        EventSerializer::publish<OrderConnectorRegisterEvent>(control_queue,
                                                              [&](OrderConnectorRegisterEvent& event) {
            event.broker_id = broker_;
            event.producer_id = order_producer_id(broker_);
            event.fee_structure = fees;
            set_shm_name(event, shm_name_);
        });
    }

    template <typename E>
    void publish(const E& event) {
        EventSerializer::publish<E>(*buffer_, [&](E& out) { out = event; });
    }

    // Hands the strategies every fill in `journal` again, before any session connects.
    // The strategy manager drops those they had before the restart.
    void replay(const FillJournal& journal, std::string_view who) {
        if (journal.size() == 0) {
            return;
        }
        auto fills = journal.fills();
        LOG_INFO("{}: replaying {} journaled fill(s); those the strategies had before the restart are dropped", who,
                 fills.size());
        for (auto& fill : fills) {
            fill.replayed = true;
            publish(fill);
        }
    }

    [[nodiscard]] BrokerId broker() const noexcept { return broker_; }

private:
    BrokerId broker_;
    std::string shm_name_;
    std::shared_ptr<slick::stream_buffer_multiplexer::producer_buffer> buffer_;
};

// Reads one request off the order request queue and, if it is for `broker`, hands it to
// `on_new(const NewOrderEvent&)`, `on_modify(const ModifyOrderEvent&)` or
// `on_cancel(const CancelOrderEvent&)`. Every connector reads every request on the shared
// queue, so each request type is filtered by broker here. Returns whether a request was
// read, for whatever broker.
template <typename New, typename Modify, typename Cancel>
bool dispatch_order_request(slick::queue<uint8_t>& queue, uint64_t& cursor, BrokerId broker, New&& on_new,
                            Modify&& on_modify, Cancel&& on_cancel) {
    auto [data, size] = queue.read(cursor);
    if (!data) {
        return false;
    }
    const auto* event = reinterpret_cast<const Event*>(data);
    if (!is_request_for(*event, broker)) {
        return true;   // another broker's
    }
    switch (event->type) {
    case EventType::NEW_ORDER:
        on_new(*reinterpret_cast<const NewOrderEvent*>(data));
        break;
    case EventType::MODIFY_ORDER:
        on_modify(*reinterpret_cast<const ModifyOrderEvent*>(data));
        break;
    case EventType::CANCEL_ORDER:
        on_cancel(*reinterpret_cast<const CancelOrderEvent*>(data));
        break;
    default:
        break;
    }
    return true;
}

} // namespace slick::trader
