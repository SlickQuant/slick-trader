#include <slick/logger.hpp>
#include <slick-trader/core/trading_engine.h>
#include <slick-trader/strategy/strategy_manager.h>
#include <slick-trader/brokers/imarket_data_connector.h>
#include <slick-trader/brokers/iorder_connector.h>
#include <nlohmann/json.hpp>
#include <fstream>

namespace slick::trader {

TradingEngine::TradingEngine()
    : running_(false)
    , strategy_manager_(std::make_unique<StrategyManager>())
    , md_queue_(nullptr)
    , order_request_queue_(nullptr)
    , order_event_queue_(nullptr) {
}

TradingEngine::~TradingEngine() {
    shutdown();
}

bool TradingEngine::initialize(const std::string& config_path) {

    LOG_INFO("Initializing TradingEngine with config: {}", config_path);

    // Load configuration
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        LOG_ERROR("Failed to open config file: {}", config_path);
        return false;
    }

    nlohmann::json config;
    try {
        config_file >> config;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse config file: {}", e.what());
        return false;
    }

    // Create message queues
    uint32_t md_queue_size = config.value("md_queue_size", 1<<24);
    uint32_t order_request_queue_size = config.value("order_request_queue_size", 1<<20);
    uint32_t order_event_queue_size = config.value("order_reply_queue_size", 1<<20);

    md_queue_ = std::make_shared<slick::SlickQueue<uint8_t>>(md_queue_size);
    order_request_queue_ = std::make_shared<slick::SlickQueue<uint8_t>>(order_request_queue_size);
    order_event_queue_ = std::make_shared<slick::SlickQueue<uint8_t>>(order_event_queue_size);

    LOG_INFO("Created message queues - MD: {}, Order: {}, OrderEvent: {}",
             md_queue_size, order_request_queue_size, order_event_queue_size);

    // Initialize strategy manager with queues
    strategy_manager_->set_market_data_queue(md_queue_);
    strategy_manager_->set_order_event_queue(order_event_queue_);
    strategy_manager_->set_order_submission_queue(order_request_queue_);

    if (!strategy_manager_->initialize()) {
        LOG_ERROR("Failed to initialize strategy manager");
        return false;
    }

    return true;
}

void TradingEngine::start() {
    if (running_.load(std::memory_order_relaxed)) {
        LOG_WARN("TradingEngine already running");
        return;
    }

    LOG_INFO("Starting TradingEngine");

    // Start strategy manager
    strategy_manager_->start();

    // Start all brokers
    for (auto& [broker_id, broker] : brokers_) {
        LOG_INFO("Starting broker: {}", broker->get_broker_name());

        auto md_connector = broker->get_market_data_connector();
        if (md_connector) {
            md_connector->set_md_queue(md_queue_);
            md_connector->start();
        }

        auto order_connector = broker->get_order_connector();
        if (order_connector) {
            order_connector->set_order_submission_queue(order_request_queue_);
            order_connector->set_order_event_queue(order_event_queue_);
            order_connector->start();
        }
    }

    running_.store(true, std::memory_order_release);
    LOG_INFO("TradingEngine started");
}

void TradingEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        LOG_WARN("TradingEngine not running");
        return;
    }

    LOG_INFO("Stopping TradingEngine");

    // Stop all brokers
    for (auto& [broker_id, broker] : brokers_) {
        LOG_INFO("Stopping broker: {}", broker->get_broker_name());

        auto md_connector = broker->get_market_data_connector();
        if (md_connector) {
            md_connector->stop();
        }

        auto order_connector = broker->get_order_connector();
        if (order_connector) {
            order_connector->stop();
        }
    }

    // Stop strategy manager
    strategy_manager_->stop();

    running_.store(false, std::memory_order_release);
    LOG_INFO("TradingEngine stopped");
}

void TradingEngine::shutdown() {
    stop();

    LOG_INFO("Shutting down TradingEngine");

    // Shutdown strategy manager
    if (strategy_manager_) {
        strategy_manager_->shutdown();
    }

    // Shutdown all brokers
    for (auto& [broker_id, broker] : brokers_) {
        LOG_INFO("Shutting down broker: {}", broker->get_broker_name());
        broker->shutdown();
    }

    brokers_.clear();
    md_queue_.reset();
    order_request_queue_.reset();
    order_event_queue_.reset();

    LOG_INFO("TradingEngine shutdown complete");
}

bool TradingEngine::register_broker(std::shared_ptr<IBroker> broker) {
    if (!broker) {
        LOG_ERROR("Cannot register null broker");
        return false;
    }

    BrokerId broker_id = broker->get_broker_id();

    if (brokers_.find(broker_id) != brokers_.end()) {
        LOG_ERROR("Broker already registered: {}", broker->get_broker_name());
        return false;
    }

    brokers_[broker_id] = broker;
    LOG_INFO("Registered broker: {} (ID: {})", broker->get_broker_name(), static_cast<int>(broker_id));

    return true;
}

std::shared_ptr<IBroker> TradingEngine::get_broker(BrokerId broker_id) {
    auto it = brokers_.find(broker_id);
    if (it != brokers_.end()) {
        return it->second;
    }

    return nullptr;
}

void TradingEngine::add_strategy(std::shared_ptr<IStrategy> strategy) {
    if (!strategy) {
        LOG_ERROR("Cannot add null strategy");
        return;
    }

    strategy_manager_->add_strategy(strategy);
    LOG_INFO("Added strategy: {}", strategy->name());
}

} // namespace slick::trader