#include <slick/logger.hpp>
#include <slick-trader/core/trading_engine.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/web/web_server.hpp>
#include <slick-trader/strategy/strategy_manager.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <slick-trader/brokers/iorder_connector.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace slick::trader {

// Helper function to join strings
static std::string join_strings(const std::vector<std::string>& vec, const std::string& delimiter) {
    if (vec.empty()) return "";
    std::ostringstream oss;
    oss << vec[0];
    for (size_t i = 1; i < vec.size(); ++i) {
        oss << delimiter << vec[i];
    }
    return oss.str();
}

TradingEngine::TradingEngine()
    : running_(false)
    , shutdown_called_(false)
    , strategy_manager_(std::make_unique<StrategyManager>())
    , position_tracker_(std::make_unique<PositionTracker>())
    , md_queue_(nullptr)
    , order_request_queue_(nullptr)
    , order_event_queue_(nullptr) {
    // Create PnLCalculator and RiskManager with position_tracker reference
    pnl_calculator_ = std::make_unique<PnLCalculator>(*position_tracker_);
    risk_manager_ = std::make_unique<RiskManager>(*position_tracker_);

    // Create WebServer with strategy manager and position/pnl trackers
    web_server_ = std::make_unique<WebServer>(*strategy_manager_, *position_tracker_, *pnl_calculator_);
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

    // Store configuration values
    strategy_plugins_folder_ = config.value("strategy_plugins_folder", "strategies");

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

    // Set position & PnL tracking
    strategy_manager_->set_position_tracker(position_tracker_.get());
    strategy_manager_->set_pnl_calculator(pnl_calculator_.get());
    strategy_manager_->set_risk_manager(risk_manager_.get());

    if (!strategy_manager_->initialize()) {
        LOG_ERROR("Failed to initialize strategy manager");
        return false;
    }

    // Initialize web server (optional, configurable)
    bool enable_web_server = config.value("enable_web_server", true);
    if (enable_web_server) {
        WebServer::Config web_config;
        web_config.port = config.value("web_server_port", 8080);
        web_config.enable_cors = config.value("web_server_cors", true);
        web_config.static_files_path = config.value("web_server_static_path", "web/static");

        if (!web_server_->initialize(web_config)) {
            LOG_ERROR("Failed to initialize web server");
            return false;
        }
    }

    return true;
}

void TradingEngine::start() {
    if (running_.load(std::memory_order_relaxed)) {
        LOG_WARN("TradingEngine already running");
        return;
    }

    LOG_INFO("Starting TradingEngine...");

    // Start web server (if initialized)
    if (web_server_) {
        web_server_->start();
    }

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

    // Stop web server
    if (web_server_) {
        web_server_->stop();
    }

    running_.store(false, std::memory_order_release);
    LOG_INFO("TradingEngine stopped");
}

void TradingEngine::shutdown() {
    // Prevent double shutdown
    bool expected = false;
    if (!shutdown_called_.compare_exchange_strong(expected, true)) {
        return; // Already shut down
    }

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

    // Set position & PnL tracking (if strategy is derived from StrategyBase)
    // This uses dynamic_cast to safely check if the strategy supports these setters
    auto strategy_base = std::dynamic_pointer_cast<StrategyBase>(strategy);
    if (strategy_base) {
        strategy_base->set_position_tracker(position_tracker_.get());
        strategy_base->set_pnl_calculator(pnl_calculator_.get());
        strategy_base->set_risk_manager(risk_manager_.get());
        strategy_base->set_order_submission_queue(order_request_queue_);
    }

    // Initialize the strategy immediately
    if (!strategy->initialize()) {
        LOG_ERROR("Failed to initialize strategy: {}", strategy->name());
        return;
    }

    // Add to strategy manager
    strategy_manager_->add_strategy(strategy);

    // If the engine is already running, start the strategy immediately (hot-loading)
    // Otherwise, it will be started when engine.start() is called
    if (strategy_manager_->is_running()) {
        strategy->start();
        LOG_INFO("Added and started strategy: {}", strategy->name());
    } else {
        LOG_INFO("Added strategy: {}", strategy->name());
    }
}

PositionTracker& TradingEngine::get_position_tracker() {
    return *position_tracker_;
}

PnLCalculator& TradingEngine::get_pnl_calculator() {
    return *pnl_calculator_;
}

RiskManager& TradingEngine::get_risk_manager() {
    return *risk_manager_;
}

WebServer& TradingEngine::get_web_server() {
    return *web_server_;
}

void TradingEngine::apply_strategy_subscriptions() {
    LOG_INFO("Applying strategy-driven market data subscriptions...");

    // Collect all subscription requests from all strategies
    std::unordered_map<BrokerId, std::unordered_map<SubscriptionType, std::vector<std::string>>> subscription_map;

    // Get all strategies from strategy manager
    for (const auto& strategy : strategy_manager_->get_all_strategies()) {
        auto subscriptions = strategy->get_subscriptions();

        for (const auto& sub : subscriptions) {
            // Add instruments to the subscription map (keyed by broker + type)
            auto& instruments = subscription_map[sub.broker_id][sub.type];
            instruments.insert(instruments.end(), sub.instruments.begin(), sub.instruments.end());

            LOG_DEBUG("Strategy {} requests {} subscription for: {}",
                     strategy->name(),
                     sub.type == SubscriptionType::TRADE ? "TRADE" :
                     sub.type == SubscriptionType::QUOTE ? "QUOTE" : "ORDERBOOK",
                     join_strings(sub.instruments, ", "));
        }
    }

    // Apply subscriptions to each broker
    int total_subscriptions = 0;
    for (const auto& [broker_id, sub_types] : subscription_map) {
        auto broker = get_broker(broker_id);
        if (!broker) {
            LOG_WARN("Broker ID {} not registered - skipping subscriptions", static_cast<int>(broker_id));
            continue;
        }

        auto md_connector = broker->get_market_data_connector();
        if (!md_connector) {
            LOG_WARN("Broker {} has no market data connector", broker->get_broker_name());
            continue;
        }

        for (const auto& [sub_type, instruments] : sub_types) {
            if (instruments.empty()) {
                continue;
            }

            // Remove duplicates
            std::vector<std::string> unique_instruments = instruments;
            std::sort(unique_instruments.begin(), unique_instruments.end());
            unique_instruments.erase(
                std::unique(unique_instruments.begin(), unique_instruments.end()),
                unique_instruments.end()
            );

            // Subscribe
            md_connector->subscribe(sub_type, unique_instruments);

            std::string type_str;
            switch (sub_type) {
                case SubscriptionType::TRADE: type_str = "trades"; break;
                case SubscriptionType::QUOTE: type_str = "quotes"; break;
                case SubscriptionType::ORDER_BOOK: type_str = "orderbook"; break;
            }

            LOG_INFO("✓ Subscribed {} to {} for: {}",
                     broker->get_broker_name(), type_str,
                     join_strings(unique_instruments, ", "));
            total_subscriptions++;
        }
    }

    if (total_subscriptions == 0) {
        LOG_WARN("⚠ No market data subscriptions from strategies");
    } else {
        LOG_INFO("✓ Applied {} strategy-driven subscription(s)", total_subscriptions);
    }
}

} // namespace slick::trader