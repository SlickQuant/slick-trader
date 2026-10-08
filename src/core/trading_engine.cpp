#include <slick/logger.hpp>
#include <slick-trader/core/trading_engine.hpp>
#include <slick-trader/core/broker_registry.hpp>
#include <slick-trader/web/web_server.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <slick-trader/brokers/iorder_connector.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick/net/logging.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace slick::trader {

using Logger = slick::logger::Logger;

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
    : pnl_calculator_(position_tracker_)
    , risk_manager_(position_tracker_)
{
}

TradingEngine::~TradingEngine() {
    shutdown();
}

bool TradingEngine::initialize(const std::string& config_path) {
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        std::cerr << "Failed to open config file: " << config_path << std::endl;
        return false;
    }

    json config;
    try {
        config = json::parse(config_file);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse config file: " << e.what() << std::endl;
        return false;
    }

    std::string log_file = config.value<std::string>("log_file_path", "logs/slick_trader.log");
    auto str_log_level = config.value<std::string>("log_level", "info");
    slick::logger::LogLevel log_level = slick::logger::to_log_level(str_log_level);

    // Initialize logger with console and file output
    auto &logger = Logger::instance();
    logger.clear_sinks();
#ifdef DEBUG
    logger.add_console_sink(true, true);
#endif
    logger.add_file_sink(log_file);
    logger.set_level(log_level);
    logger.init();

    // Forward Coinbase library logs to slick-logger
    slick::net::set_log_handler(
        [&logger](slick::net::LogLevel level, const char* format_text, std::format_args args) {
            logger.log(static_cast<slick::logger::LogLevel>(level), format_text, std::move(args));
        },
        [&logger] {
            return static_cast<slick::net::LogLevel>(logger.get_level());
        }
    );

    // Create event queue
    auto event_queue_size = config.value("event_queue_size", 1u << 24 /* 16M */);
    const char* event_queue_shm_name = config.contains("event_queue_shm_name") ? config["event_queue_shm_name"].get_ref<std::string&>().c_str() : nullptr;

    LOG_INFO("event queue size: {}", event_queue_size);
    LOG_INFO("event queue shm name: {}", event_queue_shm_name ? event_queue_shm_name : "");

    event_mux_ = std::make_unique<slick::stream_buffer_multiplexer>(event_queue_size, event_queue_shm_name);

    // Create control queue
    auto control_queue_size = config.value("control_queue_size", 1u << 20 /* 1M */);
    const char* control_queue_shm_name = config.contains("control_queue_shm_name") ? config["control_queue_shm_name"].get_ref<std::string&>().c_str() : nullptr;

    LOG_INFO("control queue size: {}", control_queue_size);
    LOG_INFO("control queue shm name: {}", control_queue_shm_name ? control_queue_shm_name : "");

    control_queue_ = std::make_unique<slick::queue<uint8_t>>(control_queue_size, control_queue_shm_name);

    // Create MD request queue
    auto md_request_queue_size = config.value("md_request_queue_size", 1u << 20 /* 1M */);
    const char* md_request_queue_shm_name = config.contains("md_request_queue_shm_name") ? config["md_request_queue_shm_name"].get_ref<std::string&>().c_str() : nullptr;

    LOG_INFO("md request queue size: {}", md_request_queue_size);
    LOG_INFO("md request queue shm name: {}", md_request_queue_shm_name ? md_request_queue_shm_name : "");

    md_request_queue_ = std::make_unique<slick::queue<uint8_t>>(md_request_queue_size, md_request_queue_shm_name);

    // Create order request queue
    auto order_request_queue_size = config.value("order_request_queue_size", 1u << 24 /* 16M */);
    const char* order_request_queue_shm_name = config.contains("order_request_queue_shm_name") ? config["order_request_queue_shm_name"].get_ref<std::string&>().c_str() : nullptr;

    LOG_INFO("order request queue size: {}", order_request_queue_size);
    LOG_INFO("order request queue shm name: {}", order_request_queue_shm_name ? order_request_queue_shm_name : "");

    order_request_queue_ = std::make_unique<slick::queue<uint8_t>>(order_request_queue_size, order_request_queue_shm_name);

    // ========================================================================
    // Broker Setup from Configuration
    // ========================================================================
    if (config.contains("brokers") && config["brokers"].is_array()) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Initialize Brokers...");
        LOG_INFO("==================================================");

        std::string built_in;
        for (const auto& key : BrokerRegistry::instance().keys()) {
            built_in += built_in.empty() ? key : ", " + key;
        }
        LOG_INFO("Broker adapters built in: {}", built_in.empty() ? "(none)" : built_in);

        for (const auto& broker_json : config["brokers"]) {
            // Parse broker configuration
            auto enabled = broker_json.value("enabled", true);
            auto broker_name = broker_json.value("name", "unknown");

            if (!enabled) {
                LOG_INFO("Skipping disabled broker: {}", broker_name);
                continue;
            }

            // An enabled broker with no adapter compiled in - a typo, or an
            // out-of-tree broker left out of SLICK_TRADER_EXTERNAL_BROKERS - is fatal
            // rather than silently skipped.
            auto factory = BrokerRegistry::instance().find(broker_name);
            if (!factory) {
                LOG_ERROR("No broker adapter for '{}'. This build has: {}",
                          broker_name, built_in.empty() ? "(none)" : built_in);
                return false;
            }

            auto broker = factory(broker_json, *event_mux_, *control_queue_, *md_request_queue_, *order_request_queue_);
            if (!broker) {
                LOG_ERROR("Failed to create broker: {}", broker_name);
                return false;
            }

            broker->register_broker();
            LOG_INFO("✓ Registered broker: {}", broker_name);

            brokers_.push_back(broker);
        }

        LOG_INFO("✓ Registered {} broker(s)", brokers_.size());

        publish_regiter_end(event_mux_->producer_count());
    } else {
        LOG_WARN("⚠ No brokers configured in {}", config_path);
        LOG_WARN("⚠ Add a 'brokers' array to enable market data and trading");
    }

    // Load strategies if configured
    if (config.contains("strategies")) {
        strategy_manager_ = std::make_unique<StrategyManager>(config["strategies"], position_tracker_, pnl_calculator_, risk_manager_, *event_mux_, *md_request_queue_, *order_request_queue_);
        if (config["strategies"].value("enabled", true)) {
            LOG_INFO("");
            LOG_INFO("==================================================");
            LOG_INFO("          Initialize Strategies...");
            LOG_INFO("==================================================");
            strategy_manager_->load_strategies(config["strategies"]);
        }
    }

    // Initialize web server (optional, configurable)
    if (config.contains("web_service") && config["web_service"].value("enabled", true)) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Initialize Web Service...");
        LOG_INFO("==================================================");
        web_server_ = std::make_unique<WebServer>(config_path, *strategy_manager_, position_tracker_, pnl_calculator_, risk_manager_, *event_mux_);
        if (!web_server_->initialize(config["web_service"])) {
            LOG_ERROR("Failed to initialize web server");
            return false;
        }
    }

    LOG_INFO("");
    LOG_INFO("==================================================");
    LOG_INFO("          Waiting for Brokers Ready...");
    LOG_INFO("==================================================");

    auto timed_out = wait_for_brokers_ready(std::chrono::seconds(config.value("waiting_time_out", 30)));
    if (timed_out) {
        LOG_WARN("Waiting for Brokers ready timed out");
    }
    return true;
}

void TradingEngine::publish_regiter_end(uint32_t num_producers) {
    EventSerializer::publish<BrokerRegisterEndEvent>(*control_queue_, [=](BrokerRegisterEndEvent &e) {
        e.num_producers = num_producers;
    });
}

bool TradingEngine::wait_for_brokers_ready(std::chrono::seconds timeout_sec) {
    uint64_t cursor{0};
    size_t num_producers{0};
    size_t num_ipc_producers{0};
    auto start = std::chrono::steady_clock::now();
    auto end = start + timeout_sec;
    bool timed_out = false;
    do {
        auto [data, size] = control_queue_->read(cursor);
        if (!data) {
            std::this_thread::yield();
        }

        auto *event = reinterpret_cast<Event*>(data);
        if (event->type == EventType::REGISTER) {
            ++num_producers;
            auto &register_event = reinterpret_cast<RegisterEvent&>(*event);
            if (register_event.shm_name[0] != '\0') {
                // add_producer() throws if the id is already taken. An out-of-process broker
                // picks its own id, so a clash with an in-process producer is possible; losing
                // that one broker's events beats taking the engine down on the way up.
                try {
                    event_mux_->add_producer(register_event.producer_id, register_event.shm_name);
                    LOG_INFO("Register IPC producer {}: borker={} type={}, shm_name={}", register_event.producer_id,
                        to_string(register_event.broker_id), to_string(register_event.component_type), register_event.shm_name);
                    ++num_ipc_producers;
                } catch (const std::exception& e) {
                    LOG_ERROR("Failed to register IPC producer {} (broker={}): {}",
                        register_event.producer_id, to_string(register_event.broker_id), e.what());
                }
            }

        } else if (event->type == EventType::REGISTER_END) {
            break;
        }
    } while (!(timed_out = std::chrono::steady_clock::now() < end));
    LOG_INFO("{} broker(s) registered, {} ipc producer(s). event_mux.producer_count={}", num_producers, num_ipc_producers, event_mux_->producer_count());
    return !timed_out;
}

void TradingEngine::start() {
    if (running_.load(std::memory_order_relaxed)) [[unlikely]] {
        LOG_WARN("TradingEngine already running");
        return;
    }

    if (!brokers_.empty()) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Start Brokers...");
        LOG_INFO("==================================================");
        for (auto &broker : brokers_) {
            broker->start();
        }
    }

    if (strategy_manager_) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Start Strategies...");
        LOG_INFO("==================================================");
        strategy_manager_->start();
    }

    // Start web server (if initialized)
    if (web_server_) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Start Web Service...");
        LOG_INFO("==================================================");
        web_server_->start();
    }

    // // Fetch and wire fee structures before strategies begin trading
    // for (auto& [broker_id, broker] : brokers_) {
    //     FeeStructure fs = broker->get_fees("");
    //     if (fs.rate > 0.0) {
    //         position_tracker_->set_fee_structure(fs);
    //         LOG_INFO("TradingEngine: {} fee structure — type={} rate={:.4f}",
    //                  broker->get_broker_name(),
    //                  fs.type == FeeType::PERCENTAGE ? "percentage" : "fixed",
    //                  fs.rate);
    //     }
    // }

    // // Start all brokers
    // for (auto& [broker_id, broker] : brokers_) {
    //     LOG_INFO("Starting broker: {}", broker->get_broker_name());

    //     auto md_connector = broker->get_market_data_connector();
    //     if (md_connector) {
    //         md_connector->set_md_queue(md_queue_);
    //         md_connector->start();
    //     }

    //     auto order_connector = broker->get_order_connector();
    //     if (order_connector) {
    //         order_connector->set_order_submission_queue(order_request_queue_);
    //         order_connector->set_order_event_queue(order_event_queue_);
    //         order_connector->start();
    //     }
    // }

    running_.store(true, std::memory_order_release);
    LOG_INFO("");
    LOG_INFO("==================================================");
    LOG_INFO("          TradingEngine started");
    LOG_INFO("==================================================");
}

void TradingEngine::shutdown() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    LOG_INFO("Stopping TradingEngine");

    // // Stop all brokers
    // for (auto& [broker_id, broker] : brokers_) {
    //     LOG_INFO("Stopping broker: {}", broker->get_broker_name());

    //     auto md_connector = broker->get_market_data_connector();
    //     if (md_connector) {
    //         md_connector->stop();
    //     }

    //     auto order_connector = broker->get_order_connector();
    //     if (order_connector) {
    //         order_connector->stop();
    //     }
    // }

    // Stop strategy manager
    if (strategy_manager_) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Stop Strategies...");
        LOG_INFO("==================================================");
        strategy_manager_->stop();
    }

    if (!brokers_.empty()) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Stop Brokers...");
        LOG_INFO("==================================================");
        for (auto &broker : brokers_) {
            broker->stop();
        }
    }

    // Stop web server
    if (web_server_) {
        LOG_INFO("");
        LOG_INFO("==================================================");
        LOG_INFO("          Stop Web Service...");
        LOG_INFO("==================================================");
        web_server_->stop();
    }

    running_.store(false, std::memory_order_release);
    LOG_INFO("TradingEngine stopped");
}

// void TradingEngine::apply_strategy_subscriptions() {
//     LOG_INFO("Applying strategy-driven market data subscriptions...");

//     // Collect all subscription requests from all strategies
//     // key: broker_id << 8 | sub.type
//     std::unordered_map<uint16_t, std::vector<std::string>> subscription_map;

//     // Get all strategies from strategy manager
//     for (const auto& strategy : strategy_manager_.get_all_strategies()) {
//         auto subscriptions = strategy->get_subscriptions();

//         for (const auto& sub : subscriptions) {
//             // Add instruments to the subscription map (keyed by broker + type)
//             auto& instruments = subscription_map[(static_cast<uint16_t>(sub.broker_id) << 8) | static_cast<uint16_t>(sub.type)];
//             instruments.insert(instruments.end(), sub.instruments.begin(), sub.instruments.end());

//             LOG_DEBUG("Strategy {} requests {} subscription for: {}",
//                      strategy->name(),
//                      sub.type == SubscriptionType::TRADE ? "TRADE" :
//                      sub.type == SubscriptionType::QUOTE ? "QUOTE" : 
//                      sub.type == SubscriptionType::LEVEL2 ? "LEVEL2" : "ORDERBOOK",
//                      join_strings(sub.instruments, ", "));
//         }
//     }

//     // Apply subscriptions to each broker
//     int total_subscriptions = 0;
//     for (const auto& [key, instruments] : subscription_map) {
//         auto broker_id = static_cast<BrokerId>(key >> 8);
//         auto broker = get_broker(broker_id);
//         if (!broker) {
//             LOG_WARN("Broker ID {} not registered - skipping subscriptions", static_cast<int>(broker_id));
//             continue;
//         }

//         auto md_connector = broker->get_market_data_connector();
//         if (!md_connector) {
//             LOG_WARN("Broker {} has no market data connector", broker->get_broker_name());
//             continue;
//         }

//         // Remove duplicates
//         std::vector<std::string> unique_instruments = instruments;
//         std::sort(unique_instruments.begin(), unique_instruments.end());
//         unique_instruments.erase(
//             std::unique(unique_instruments.begin(), unique_instruments.end()),
//             unique_instruments.end()
//         );

//         // Subscribe
//         auto sub_type = static_cast<SubscriptionType>(key & 0xFF);
//         md_connector->subscribe(sub_type, unique_instruments);

//         std::string type_str;
//         switch (sub_type) {
//             case SubscriptionType::TRADE: type_str = "trades"; break;
//             case SubscriptionType::QUOTE: type_str = "quotes"; break;
//             case SubscriptionType::LEVEL2: {
//                 type_str = "level2";
//                 strategy_manager_->subscribe_book_update(unique_instruments);
//                 break;
//             }
//             case SubscriptionType::ORDER_BOOK: {
//                 type_str = "orderbook";
//                 strategy_manager_->subscribe_book_update(unique_instruments);
//                 break;
//             }
//         }

//         LOG_INFO("✓ Subscribed {} to {} for: {}",
//                     broker->get_broker_name(), type_str,
//                     join_strings(unique_instruments, ", "));
//         total_subscriptions += unique_instruments.size();
//     }

//     if (total_subscriptions == 0) {
//         LOG_WARN("⚠ No market data subscriptions from strategies");
//     } else {
//         LOG_INFO("✓ Applied {} strategy-driven subscription(s)", total_subscriptions);
//     }
// }

} // namespace slick::trader