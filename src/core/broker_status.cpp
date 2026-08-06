#include <slick/logger.hpp>
#include <slick-trader/core/broker_status.hpp>

using namespace slick::trader;

void BrokerStatusRegistry::set_md_enabled(BrokerId id, bool v) {
    slot(id).md_enabled.store(v, std::memory_order_release);
    if (v) {
        LOG_INFO("BrokerStatusRegistry: {} MD enabled", slot(id).name);
    } else {
        LOG_WARN("BrokerStatusRegistry: {} MD disabled", slot(id).name);
    }
}

void BrokerStatusRegistry::set_md_connected(BrokerId id, bool v) {
    slot(id).md_connected.store(v, std::memory_order_release);
    if (v) {
        LOG_INFO("BrokerStatusRegistry: {} MD connected", slot(id).name);
    } else {
        LOG_WARN("BrokerStatusRegistry: {} MD disconnected", slot(id).name);
    }
}

void BrokerStatusRegistry::set_order_enabled(BrokerId id, bool v) {
    slot(id).order_enabled.store(v, std::memory_order_release);
    if (v) {
        LOG_INFO("BrokerStatusRegistry: {} Order enabled", slot(id).name);
    } else {
        LOG_WARN("BrokerStatusRegistry: {} Order disabled", slot(id).name);
    }
}

void BrokerStatusRegistry::set_order_connected(BrokerId id, bool v){
    slot(id).order_connected.store(v, std::memory_order_release);
    if (v) {
        LOG_INFO("BrokerStatusRegistry: {} Order connected", slot(id).name);
    } else {
        LOG_WARN("BrokerStatusRegistry: {} Order disconnected", slot(id).name);
    }
}