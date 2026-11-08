// Regression tests: what StrategyManager asks the brokers to subscribe.
//
// Three bugs lived in subscribe()/unsubscribe():
//   - Subscriptions were counted by instrument name alone, so the second kind of data for an
//     instrument was taken for one already held and never sent: a strategy subscribing the
//     book and then the trades of BTC-USD got no trade subscription. The same went for the
//     same name at a second broker.
//   - The loop writing the instruments into the request never advanced its slot: every name
//     was written over slot 0 and the other slots went out as whatever the queue held.
//   - unsubscribe() sent its request only when there was nothing to unsubscribe.

#include <gtest/gtest.h>

#include "log_count.hpp"
#include "strategy_manager_fixture.hpp"

#include <slick-trader/brokers/common/md_event_writer.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/strategy_config.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/instrument_definition_event.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <slick-trader/events/strategy_config_event.hpp>
#include <slick-trader/strategy/manual_strategy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace slick::trader;

namespace {

class SubscribingStrategy : public StrategyBase {
public:
    SubscribingStrategy(const std::string& name, std::vector<SubscriptionRequest> subscriptions)
        : StrategyBase(name, "1.0.0"), subscriptions_(std::move(subscriptions)) {}

    std::vector<SubscriptionRequest> get_subscriptions() const override { return subscriptions_; }

private:
    std::vector<SubscriptionRequest> subscriptions_;
};

// One request as a broker's market data connector reads it off the queue.
struct Request {
    EventType type;
    BrokerId broker;
    SubscriptionType subscription;
    std::vector<std::string> instruments;
};

// MANUAL's config parameters, listing `symbols` to adopt as brokers define them.
nlohmann::json manual_lists(std::vector<std::string> symbols) {
    return {{"instruments", std::move(symbols)}};
}

class MdSubscriptionTest : public ::testing::Test, protected test::StrategyManagerFixture {
protected:
    void add(const std::string& name, std::vector<SubscriptionRequest> subscriptions) {
        add_strategy(std::make_shared<SubscribingStrategy>(name, std::move(subscriptions)));
    }

    // Every request published since the last call, broker by broker.
    std::vector<Request> requests() {
        std::vector<Request> out;
        for (size_t b = 1; b < MdRequestQueues::BROKER_COUNT; ++b) {
            auto& queue = md_qs_.get(static_cast<BrokerId>(b));
            for (;;) {
                auto [data, size] = queue.read(cursors_[b]);
                if (data == nullptr) {
                    break;
                }
                const auto& request = *reinterpret_cast<const MDSubscription*>(data);
                EXPECT_EQ(size, sizeof(MDSubscription) + request.num_instruments * sizeof(char[32]));
                EXPECT_EQ(request.broker_id, static_cast<BrokerId>(b)) << "on another broker's queue";
                Request r{request.type, request.broker_id, request.subscription_type, {}};
                for (uint32_t i = 0; i < request.num_instruments; ++i) {
                    r.instruments.emplace_back(request.instruments[i]);
                }
                out.push_back(std::move(r));
            }
        }
        return out;
    }

    // A manager of its own, holding MANUAL, for a test that needs a config or request queues
    // other than the fixture's.
    std::unique_ptr<StrategyManager> manual_manager(MdRequestQueues& md_qs, nlohmann::json config,
                                                    const nlohmann::json& manual_params = manual_lists({"*"})) {
        config["TOB_queue_size"] = 65536;
        config["TOB_control_size"] = 4096;
        auto manager = std::make_unique<StrategyManager>(config, positions_, pnl_, risk_, mux_, md_qs, order_q_);
        auto manual = std::make_shared<ManualStrategy>();
        manual->initialize(manual_params, positions_, pnl_, risk_, order_q_);
        manager->add_strategy(manual);
        return manager;
    }

    std::array<uint64_t, MdRequestQueues::BROKER_COUNT> cursors_{};   // the queues are new: each starts at 0
};

} // namespace

TEST_F(MdSubscriptionTest, EachKindOfDataForAnInstrumentReachesTheBroker) {
    // What market_maker and order_stacker ask for: the book, then the trades, of one instrument.
    add("Maker", {SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::LEVEL2, {"BTC-USD"}),
                  SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, {"BTC-USD"})});
    manager_.start();

    const auto sent = requests();
    ASSERT_EQ(sent.size(), 2u) << "the trade subscription was dropped as a duplicate of the book's";
    EXPECT_EQ(sent[0].type, EventType::SUBSCRIBE);
    EXPECT_EQ(sent[0].subscription, SubscriptionType::LEVEL2);
    EXPECT_EQ(sent[1].type, EventType::SUBSCRIBE);
    EXPECT_EQ(sent[1].subscription, SubscriptionType::TRADE);
    EXPECT_EQ(sent[1].instruments, std::vector<std::string>{"BTC-USD"});
}

TEST_F(MdSubscriptionTest, TheSameNameAtTwoBrokersIsSubscribedAtBoth) {
    add("Arb", {SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, {"BTC"}),
                SubscriptionRequest(BrokerId::HYPERLIQUID, SubscriptionType::TRADE, {"BTC"})});
    manager_.start();

    const auto sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].broker, BrokerId::COINBASE);
    EXPECT_EQ(sent[1].broker, BrokerId::HYPERLIQUID);
}

TEST_F(MdSubscriptionTest, EveryInstrumentOfARequestGetsItsOwnSlot) {
    const std::vector<std::string> instruments{"BTC-USD", "ETH-USD", "SOL-USD"};
    add("Basket", {SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, instruments)});
    manager_.start();

    const auto sent = requests();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].instruments, instruments) << "every name was written to the first slot";
}

TEST_F(MdSubscriptionTest, ASubscriptionTwoStrategiesShareIsSentOnceAndDroppedOnce) {
    add("First", {SubscriptionRequest(BrokerId::CME, SubscriptionType::MBO, {"ESZ6"})});
    add("Second", {SubscriptionRequest(BrokerId::CME, SubscriptionType::MBO, {"ESZ6", "NQZ6"})});
    manager_.start();

    auto sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].instruments, std::vector<std::string>{"ESZ6"});
    EXPECT_EQ(sent[1].instruments, std::vector<std::string>{"NQZ6"}) << "ESZ6 is already subscribed";

    manager_.stop();

    // The first strategy lets go of ESZ6 while the second still holds it: nothing to send.
    sent = requests();
    ASSERT_EQ(sent.size(), 1u) << "unsubscribe() published only when it had nothing to unsubscribe";
    EXPECT_EQ(sent[0].type, EventType::UNSUBSCRIBE);
    EXPECT_EQ(sent[0].broker, BrokerId::CME);
    EXPECT_EQ(sent[0].subscription, SubscriptionType::MBO);
    EXPECT_EQ(sent[0].instruments, (std::vector<std::string>{"ESZ6", "NQZ6"}));
}

// MANUAL used to subscribe nothing, so with no strategy plugin loaded the Web UI ladder had
// no instrument. It now adopts every instrument a broker defines and holds its level 2 book.

namespace {

InstrumentDefinitionEvent definition(BrokerId broker, const char* instrument) {
    InstrumentDefinitionEvent event;
    event.broker_id = broker;
    event.set_instrument(instrument);
    return event;
}

// How many instruments MANUAL holds.
uint32_t adopted(const StrategyManager& manager) {
    return manager.manual()->adopted_count();
}

} // namespace

TEST_F(MdSubscriptionTest, ManualAdoptsWhatBrokersDefineAndSubscribesItsBookAtStart) {
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"ZNZ6", "BRNF7"}));

    const InstrumentDefinitionEvent defined[] = {definition(BrokerId::CME, "ZNZ6"),
                                                 definition(BrokerId::CME, "ZNZ6"),
                                                 definition(BrokerId::ICE, "BRNF7")};
    for (const auto& event : defined) {
        manager_.process_event(&event);
    }
    EXPECT_EQ(adopted(manager_), 2u) << "a definition seen again is adopted once";
    EXPECT_TRUE(requests().empty()) << "nothing goes to a broker before the manager starts";

    manager_.start();
    auto sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].type, EventType::SUBSCRIBE);
    EXPECT_EQ(sent[0].broker, BrokerId::CME);
    EXPECT_EQ(sent[0].subscription, SubscriptionType::LEVEL2);
    EXPECT_EQ(sent[0].instruments, std::vector<std::string>{"ZNZ6"});
    EXPECT_EQ(sent[1].broker, BrokerId::ICE);
    EXPECT_EQ(sent[1].instruments, std::vector<std::string>{"BRNF7"});

    manager_.stop();
    sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].type, EventType::UNSUBSCRIBE);
    EXPECT_EQ(sent[1].type, EventType::UNSUBSCRIBE);
}

TEST_F(MdSubscriptionTest, ManualSubscribesAnInstrumentDefinedWhileRunning) {
    auto manual = std::make_shared<ManualStrategy>();
    add_strategy(manual, manual_lists({"ZNZ6"}));
    auto producer = mux_.add_producer(mux_.producer_count(), 1u << 16, 4096);
    manager_.start();
    ASSERT_TRUE(requests().empty());

    // Through the bus, so the worker thread adopts it, as it does a broker's definition.
    EventSerializer::publish<InstrumentDefinitionEvent>(*producer, [](InstrumentDefinitionEvent& event) {
        event.broker_id = BrokerId::CME;
        event.set_instrument("ZNZ6");
    });

    std::vector<Request> sent;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sent.empty() && std::chrono::steady_clock::now() < deadline) {
        sent = requests();
        std::this_thread::yield();
    }
    ASSERT_EQ(sent.size(), 1u) << "the worker never subscribed the instrument";
    EXPECT_EQ(sent[0].type, EventType::SUBSCRIBE);
    EXPECT_EQ(sent[0].broker, BrokerId::CME);
    EXPECT_EQ(sent[0].subscription, SubscriptionType::LEVEL2);
    EXPECT_EQ(sent[0].instruments, std::vector<std::string>{"ZNZ6"});

    // Read off this thread while the worker runs, as the WebServer reads it.
    const auto held = manual->get_subscriptions();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].instruments, std::vector<std::string>{"ZNZ6"});

    manager_.stop();
    EXPECT_EQ(adopted(manager_), 1u);
    sent = requests();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].type, EventType::UNSUBSCRIBE);
}

// A whole channel is more than the request queue holds, and the queue is lossy.

namespace {

// A broker's market data connector as the manager sees one: enabled in BrokerStatusRegistry,
// reading its request queue on its own thread through process_md_request(), which reports
// how far it has read. It spends `per_request` on each request of its own, as a connector
// does subscribing at its venue. Paused, it reads nothing, as one that stopped or hung.
class Connector {
public:
    Connector(slick::queue<uint8_t>& queue, BrokerId broker, std::chrono::milliseconds per_request = {},
              bool paused = false)
        : queue_(queue), broker_(broker), per_request_(per_request), cursor_(queue.initial_reading_index()),
          paused_(paused) {
        auto& registry = BrokerStatusRegistry::instance();
        registry.set_md_request_cursor(broker_, BrokerStatus::NO_MD_REQUEST_CURSOR);
        registry.set_md_enabled(broker_, true);
        thread_ = std::thread([this] { run(); });
    }

    ~Connector() {
        stop();
        auto& registry = BrokerStatusRegistry::instance();
        registry.set_md_enabled(broker_, false);
        registry.set_md_request_cursor(broker_, BrokerStatus::NO_MD_REQUEST_CURSOR);
    }

    // Returns once it has read everything published before the call, or at once if paused.
    void stop() {
        if (thread_.joinable()) {
            stopping_.store(true, std::memory_order_release);
            thread_.join();
        }
    }

    void resume() { paused_.store(false, std::memory_order_release); }

    size_t subscribed_count() const { return subscribed_count_.load(std::memory_order_acquire); }

    // Read after stop().
    std::multiset<std::string> subscribed;
    std::multiset<std::string> unsubscribed;
    size_t subscribe_requests = 0;
    size_t largest_request = 0;

private:
    void run() {
        auto take = [this](const MDSubscription& request, std::multiset<std::string>& into) {
            // Before reading it: the request must still be there when the connector gets to it.
            std::this_thread::sleep_for(per_request_);
            const uint32_t n = request.num_instruments;
            largest_request = std::max(largest_request, sizeof(MDSubscription) + n * sizeof(char[32]));
            for (uint32_t i = 0; i < n; ++i) {
                into.emplace(request.instruments[i]);
            }
        };
        for (;;) {
            const bool stopping = stopping_.load(std::memory_order_acquire);
            const bool read = !paused_.load(std::memory_order_acquire) && process_md_request(
                queue_, cursor_, broker_,
                [&](const MDSubscription& request) {
                    take(request, subscribed);
                    ++subscribe_requests;
                    subscribed_count_.fetch_add(request.num_instruments, std::memory_order_release);
                },
                [&](const MDSubscription& request) { take(request, unsubscribed); });
            if (!read) {
                if (stopping) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    }

    slick::queue<uint8_t>& queue_;
    const BrokerId broker_;
    const std::chrono::milliseconds per_request_;
    uint64_t cursor_;
    std::atomic<bool> paused_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> subscribed_count_{0};
    std::thread thread_;
};

std::string instrument_name(size_t i) { return "I" + std::to_string(i); }

// Has `manager` handle `count` definitions from `broker`, named from instrument_name(from) on,
// and returns the names.
std::multiset<std::string> define_channel(StrategyManager& manager, BrokerId broker, size_t from, size_t count) {
    std::multiset<std::string> names;
    for (size_t i = from; i < from + count; ++i) {
        names.insert(instrument_name(i));
        const auto event = definition(broker, instrument_name(i).c_str());
        manager.process_event(&event);
    }
    return names;
}

bool wait_until(const std::chrono::steady_clock::time_point deadline, const auto& done) {
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace

// 2,048 instruments make five requests each way, more than the 64 KiB queue holds, and
// start() and stop() published them all at once: a connector taking a moment over each
// request lost some of both. Requests now wait until no connector would lose them.
TEST_F(MdSubscriptionTest, ASlowConnectorGetsEveryRequestOfAChannel) {
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"*"}));
    constexpr size_t COUNT = 2048;
    const auto defined = define_channel(manager_, BrokerId::CME, 0, COUNT);

    Connector cme(md_qs_.get(BrokerId::CME), BrokerId::CME, std::chrono::milliseconds(20));
    manager_.start();
    EXPECT_TRUE(wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(10),
                           [&] { return cme.subscribed_count() >= COUNT; }))
        << "a subscription request was overwritten before the connector read it";
    EXPECT_NO_THROW(manager_.stop());
    cme.stop();

    EXPECT_EQ(cme.subscribed, defined);
    EXPECT_EQ(cme.unsubscribed, defined);
    EXPECT_GT(cme.subscribe_requests, 1u);
    EXPECT_LE(cme.largest_request, md_qs_.get(BrokerId::CME).size() / 4);
}

// Adoptions went out a request at a time, holding only as far as the next instrument of
// another broker, so CME and ICE definitions in turn were subscribed one per 50 ms: a
// channel took over 100 seconds. They are now grouped by broker.
TEST_F(MdSubscriptionTest, InterleavedDefinitionsAreSubscribedByBroker) {
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"*"}));
    auto producer = mux_.add_producer(mux_.producer_count(), 1u << 16, 4096);
    Connector cme(md_qs_.get(BrokerId::CME), BrokerId::CME);
    Connector ice(md_qs_.get(BrokerId::ICE), BrokerId::ICE);
    manager_.start();

    constexpr size_t COUNT = 2048;
    std::multiset<std::string> defined[2];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (size_t i = 0; i < COUNT; ++i) {
        const BrokerId broker = i % 2 == 0 ? BrokerId::CME : BrokerId::ICE;
        defined[i % 2].insert(instrument_name(i));
        EventSerializer::publish<InstrumentDefinitionEvent>(*producer, [&](InstrumentDefinitionEvent& event) {
            event.broker_id = broker;
            event.set_instrument(instrument_name(i));
        });
        // One at a time, so the test's own bus never laps the worker.
        ASSERT_TRUE(wait_until(deadline, [&] { return adopted(manager_) > i; }));
    }
    EXPECT_TRUE(wait_until(deadline, [&] { return cme.subscribed_count() + ice.subscribed_count() >= COUNT; }))
        << "subscribed " << cme.subscribed_count() + ice.subscribed_count() << " of " << COUNT;
    manager_.stop();
    cme.stop();
    ice.stop();

    EXPECT_EQ(cme.subscribed, defined[0]);
    EXPECT_EQ(ice.subscribed, defined[1]);
    EXPECT_EQ(cme.unsubscribed, defined[0]);
    EXPECT_EQ(ice.unsubscribed, defined[1]);
    EXPECT_LT(cme.subscribe_requests + ice.subscribe_requests, COUNT / 4);
    EXPECT_LE(std::max(cme.largest_request, ice.largest_request), md_qs_.get(BrokerId::CME).size() / 4);
}

// Requests shared one queue, so a connector that stopped reading held up every broker's until it
// was given up on, and from then on the manager wrote over what it had not read: back and
// reading, it never saw some of its own, which stayed subscribed here and streamed nothing. Each
// broker now has a queue of its own, and its requests wait for its connector however long it takes.
TEST_F(MdSubscriptionTest, AConnectorThatStopsReadingLosesNoRequestAndHoldsUpNoOtherBroker) {
    auto manager = manual_manager(md_qs_, {{"md_request_stall_timeout_ms", 200}});
    constexpr size_t COUNT = 2048;
    const auto cme_defined = define_channel(*manager, BrokerId::CME, 0, COUNT);
    const auto ice_defined = define_channel(*manager, BrokerId::ICE, COUNT, COUNT);

    Connector cme(md_qs_.get(BrokerId::CME), BrokerId::CME, {}, true);
    Connector ice(md_qs_.get(BrokerId::ICE), BrokerId::ICE);
    manager->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    EXPECT_TRUE(wait_until(deadline, [&] { return ice.subscribed_count() >= COUNT; }))
        << "subscribed " << ice.subscribed_count() << " of " << COUNT << " behind a connector reading nothing";

    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // past md_request_stall_timeout_ms
    cme.resume();
    EXPECT_TRUE(wait_until(deadline, [&] { return cme.subscribed_count() >= COUNT; }))
        << "subscribed " << cme.subscribed_count() << " of " << COUNT;
    manager->stop();
    cme.stop();
    ice.stop();

    EXPECT_EQ(cme.subscribed, cme_defined);
    EXPECT_EQ(cme.unsubscribed, cme_defined);
    EXPECT_EQ(ice.subscribed, ice_defined);
    EXPECT_EQ(ice.unsubscribed, ice_defined);
}

// stop() waits on a connector still reading its unsubscribes, but not on one that reads none:
// its broker is stopping too.
TEST_F(MdSubscriptionTest, StopGivesUpOnAConnectorThatReadsNothing) {
    auto manager = manual_manager(md_qs_, {{"md_request_stall_timeout_ms", 100}});
    define_channel(*manager, BrokerId::CME, 0, 2048);   // more than CME's queue holds
    Connector cme(md_qs_.get(BrokerId::CME), BrokerId::CME, {}, true);
    manager->start();

    const auto stopping = std::chrono::steady_clock::now();
    manager->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stopping, std::chrono::seconds(3));
    cme.stop();
}

// stop() gave up on a connector reading nothing and kept it marked as reported. Started again,
// the manager never said its requests waited on it, and the next stop() gave up on it at once.
TEST_F(MdSubscriptionTest, AConnectorGivenUpOnIsReportedAgainAfterARestart) {
    auto manager = manual_manager(md_qs_, {{"md_request_stall_timeout_ms", 100}});
    define_channel(*manager, BrokerId::CME, 0, 2048);   // more than CME's queue holds
    Connector cme(md_qs_.get(BrokerId::CME), BrokerId::CME, {}, true);
    manager->start();
    manager->stop();   // gives up on it, reported

    slick::logger::Logger::instance().flush();   // that report is not counted below
    const test::LogCount stalled("has read no request in");
    manager->start();
    EXPECT_TRUE(wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(5),
                           [&] { return stalled.count() > 0; }))
        << "the requests of a connector stop() gave up on were not reported waiting";
    manager->stop();
    cme.stop();
}

// Requests went out in one line, so one waiting on a connector held up every request behind
// it, whatever its broker. Each broker's now waits only on its own connector.
TEST_F(MdSubscriptionTest, ARequestWaitingOnOneConnectorHoldsUpNoOtherBroker) {
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"*"}));
    auto producer = mux_.add_producer(mux_.producer_count(), 1u << 16, 4096);
    constexpr size_t COUNT = 2048;
    // More of ICE's book than its queue holds, while CME's connector reads nothing.
    auto ice_defined = define_channel(manager_, BrokerId::ICE, 0, COUNT);
    Connector cme(md_qs_.get(BrokerId::CME), BrokerId::CME, {}, true);
    Connector ice(md_qs_.get(BrokerId::ICE), BrokerId::ICE);
    manager_.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    ASSERT_TRUE(wait_until(deadline, [&] { return ice.subscribed_count() >= COUNT; }));

    // One more for each, CME's first: it waits for CME's connector, and ICE's goes past it.
    auto define = [&](BrokerId broker, const char* instrument) {
        EventSerializer::publish<InstrumentDefinitionEvent>(*producer, [&](InstrumentDefinitionEvent& event) {
            event.broker_id = broker;
            event.set_instrument(instrument);
        });
    };
    define(BrokerId::CME, "C0");
    define(BrokerId::ICE, "J0");
    ice_defined.insert("J0");
    EXPECT_TRUE(wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(3),
                           [&] { return ice.subscribed_count() > COUNT; }))
        << "ICE's request waited behind CME's";

    cme.resume();
    EXPECT_TRUE(wait_until(deadline, [&] { return cme.subscribed_count() == 1; }));
    manager_.stop();
    cme.stop();
    ice.stop();

    EXPECT_EQ(cme.subscribed, std::multiset<std::string>{"C0"});
    EXPECT_EQ(cme.unsubscribed, std::multiset<std::string>{"C0"});
    EXPECT_EQ(ice.subscribed, ice_defined);
    EXPECT_EQ(ice.unsubscribed, ice_defined);
}

// A queue smaller than twice a one-instrument request never had room for one by the
// manager's reckoning, so its requests waited for good. Any request fits once every connector
// has caught up.
TEST_F(MdSubscriptionTest, ATinyRequestQueueStillCarriesARequest) {
    MdRequestQueues tiny(128);
    ASSERT_GT(2 * (sizeof(MDSubscription) + sizeof(char[32])), tiny.get(BrokerId::CME).size());
    auto manager = manual_manager(tiny, nlohmann::json::object());
    const auto event = definition(BrokerId::CME, "ZNZ6");
    manager->process_event(&event);

    Connector cme(tiny.get(BrokerId::CME), BrokerId::CME);
    manager->start();
    EXPECT_TRUE(wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(5),
                           [&] { return cme.subscribed_count() == 1; }));
    manager->stop();
    cme.stop();

    EXPECT_EQ(cme.subscribed, std::multiset<std::string>{"ZNZ6"});
    EXPECT_EQ(cme.unsubscribed, std::multiset<std::string>{"ZNZ6"});
}

// The dashboards learned of what MANUAL adopted by fetching all it held, again and again. They are
// handed what is new: what it adopted from the count they were given last.
TEST(ManualStrategyTest, HandsOverWhatItAdoptedSinceACount) {
    ManualStrategy manual;
    manual.set_config(manual_lists({"*"}));
    ASSERT_TRUE(manual.adopt(BrokerId::CME, "ZNZ6"));
    ASSERT_TRUE(manual.adopt(BrokerId::ICE, "BRNF7"));

    std::vector<std::pair<BrokerId, std::string>> seen;
    const auto collect = [&](BrokerId broker, std::string_view instrument) { seen.emplace_back(broker, instrument); };
    uint32_t count = manual.for_each_adopted(0, collect);
    EXPECT_EQ(count, 2u);
    ASSERT_TRUE(manual.adopt(BrokerId::CME, "ZBZ6"));
    count = manual.for_each_adopted(count, collect);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(manual.for_each_adopted(count, collect), 3u);
    EXPECT_EQ(seen, (std::vector<std::pair<BrokerId, std::string>>{
                        {BrokerId::CME, "ZNZ6"}, {BrokerId::ICE, "BRNF7"}, {BrokerId::CME, "ZBZ6"}}));
    EXPECT_EQ(manual.adopted_count(), 3u);
}

// Full, MANUAL turned an instrument down but still took it for held. Only adopt() reads what
// it takes for held, so that cannot be seen from outside: this pins down the limit itself.
// And it logged each one it turned down, a venue's worth of lines: it says so once now.
TEST(ManualStrategyTest, HoldsNoMoreThanItsMaximum) {
    ManualStrategy manual;
    manual.set_config(manual_lists({"*"}));
    for (uint32_t i = 0; i < ManualStrategy::MAX_INSTRUMENTS; ++i) {
        ASSERT_TRUE(manual.adopt(BrokerId::CME, instrument_name(i)));
    }
    const test::LogCount full("holding the maximum of");
    EXPECT_FALSE(manual.adopt(BrokerId::CME, "OVER"));
    EXPECT_FALSE(manual.adopt(BrokerId::CME, "OVER"));
    EXPECT_FALSE(manual.adopt(BrokerId::ICE, "OVER2"));
    EXPECT_FALSE(manual.adopt(BrokerId::CME, instrument_name(0))) << "held already";
    EXPECT_EQ(full.count(), 1u);

    const auto held = manual.get_subscriptions();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].instruments.size(), ManualStrategy::MAX_INSTRUMENTS);
    EXPECT_EQ(std::count(held[0].instruments.begin(), held[0].instruments.end(), "OVER"), 0);
}

// A request a connector could not read was dropped with nothing said: one lapped, another
// broker's, or of no request type. Each is logged now.
TEST(ProcessMdRequestTest, ARequestItCannotReadIsLogged) {
    constexpr BrokerId BROKER = BrokerId::ALPACA;
    MdRequestQueues queues(1024);
    auto& queue = queues.get(BROKER);
    uint64_t cursor = queue.initial_reading_index();
    const auto write = [&](BrokerId broker, EventType type) {
        const uint64_t index = queue.reserve(sizeof(MDSubscription));
        auto& request = *new (queue[index]) MDSubscription();
        request.type = type;
        request.broker_id = broker;
        request.num_instruments = 0;
        queue.publish(index, sizeof(MDSubscription));
    };
    write(BrokerId::ICE, EventType::SUBSCRIBE);
    write(BROKER, EventType::TOB);

    const test::LogCount dropped("market data request dropped");
    const auto handle = [](const MDSubscription&) { ADD_FAILURE() << "a request it could not read was handled"; };
    EXPECT_TRUE(process_md_request(queue, cursor, BROKER, handle, handle));
    EXPECT_TRUE(process_md_request(queue, cursor, BROKER, handle, handle));
    EXPECT_FALSE(process_md_request(queue, cursor, BROKER, handle, handle));
    EXPECT_EQ(dropped.count(), 2u);
    BrokerStatusRegistry::instance().set_md_request_cursor(BROKER, BrokerStatus::NO_MD_REQUEST_CURSOR);
}

// A MANUAL config file with no "parameters" gave MANUAL a null config, and the Web UI got null
// back for its parameters. A config's parameters are an object, empty when the file has none.
TEST(StrategyConfigTest, ParametersAreAnObjectWhenTheFileHasNone) {
    EXPECT_EQ(StrategyConfig::from_json({{"name", "MANUAL"}}).parameters, nlohmann::json::object());
    EXPECT_EQ(StrategyConfig::from_json({{"name", "MANUAL"}, {"parameters", nullptr}}).parameters,
              nlohmann::json::object());
    EXPECT_EQ(StrategyConfig::from_json({{"name", "MANUAL"}, {"parameters", manual_lists({"ZN*"})}}).parameters,
              manual_lists({"ZN*"}));
}

namespace {

// Records each level 2 book it is handed, as its instrument and best bid, and the instrument of
// each quote.
class BookWatcher : public SubscribingStrategy {
public:
    using SubscribingStrategy::SubscribingStrategy;

    void on_order_book(std::string_view instrument, const slick::orderbook::OrderBookL2& book) override {
        const auto* best = book.getLevelByIndex(slick::orderbook::Side::Buy, 0);
        books.emplace_back(std::string(instrument), best ? static_cast<price_t>(best->price) : NULL_PRICE);
        count.fetch_add(1, std::memory_order_release);
    }

    void on_quote(const QuoteEvent& event) override { quotes.insert(event.instrument); }

    std::vector<std::pair<std::string, price_t>> books;   // the worker's; read after stop()
    std::set<std::string> quotes;
    std::atomic<size_t> count{0};
};

} // namespace

// on_order_book() named no instrument, which was safe only while a strategy got the books it
// subscribed and no other. Each book now comes with its name.
//
// And MANUAL's subscriptions kept a book here for every instrument a broker defines, each taking
// one of 65,536 book ids: past that, two instruments shared a book. They kept quotes and marks
// coming for the whole venue too. MANUAL's instruments now stream to the Web UI alone.
TEST_F(MdSubscriptionTest, BooksAreKeptForStrategiesAndComeWithTheirInstrument) {
    auto watcher = std::make_shared<BookWatcher>(
        "Watcher", std::vector{SubscriptionRequest(BrokerId::CME, SubscriptionType::LEVEL2, {"ZNZ6"})});
    add_strategy(watcher);
    add("Other", {SubscriptionRequest(BrokerId::CME, SubscriptionType::LEVEL2, {"ZBZ6"})});
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"*"}));
    const auto adopted = definition(BrokerId::CME, "ZTZ6");   // MANUAL's alone
    manager_.process_event(&adopted);
    auto producer = mux_.add_producer(mux_.producer_count(), 1u << 16, 4096);
    manager_.start();

    auto publish = [&](const char* instrument, double bid) {
        EventSerializer::publish<Level2UpdateEvent>(
            *producer, sizeof(Level2UpdateEvent) + 2 * sizeof(PriceLevel), [&](Level2UpdateEvent& event) {
                event.broker_id = BrokerId::CME;
                event.set_instrument(instrument);
                event.is_snapshot = true;
                event.num_levels = 2;
                event.levels[0] = PriceLevel(to_price_t(bid), to_qty_t(1.0), Side::BUY);
                event.levels[1] = PriceLevel(to_price_t(bid + 0.25), to_qty_t(1.0), Side::SELL);
            });
    };
    publish("ZTZ6", 112.0);   // first, so it has been handled by the time the others arrive
    publish("ZNZ6", 110.5);
    publish("ZBZ6", 115.25);
    EXPECT_TRUE(wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(5),
                           [&] { return watcher->count.load(std::memory_order_acquire) >= 4; }));
    manager_.stop();

    // A level at a time, each one a callback: bid then ask, of each book a strategy subscribed.
    ASSERT_EQ(watcher->books.size(), 4u);
    EXPECT_EQ(watcher->books[0], std::pair(std::string("ZNZ6"), to_price_t(110.5)));
    EXPECT_EQ(watcher->books[1].first, "ZNZ6");
    EXPECT_EQ(watcher->books[2], std::pair(std::string("ZBZ6"), to_price_t(115.25)));
    EXPECT_EQ(watcher->books[3].first, "ZBZ6");
    EXPECT_EQ(watcher->quotes, (std::set<std::string>{"ZNZ6", "ZBZ6"})) << "MANUAL's ZTZ6 was quoted";
}

// MANUAL adopted every instrument a broker defined: a venue's worth of subscriptions. It now
// adopts what its config lists, by symbol or by a prefix ending in '*'.
TEST_F(MdSubscriptionTest, ManualAdoptsOnlyWhatItsConfigLists) {
    auto manager = manual_manager(md_qs_, nlohmann::json::object(), manual_lists({"ZNZ6", "BRN*"}));
    for (const auto& [broker, instrument] : {std::pair{BrokerId::CME, "ZNZ6"}, std::pair{BrokerId::CME, "ZBZ6"},
                                             std::pair{BrokerId::ICE, "BRNF7"}, std::pair{BrokerId::ICE, "BRNG7"},
                                             std::pair{BrokerId::ICE, "GASF7"}}) {
        const auto event = definition(broker, instrument);
        manager->process_event(&event);
    }
    EXPECT_EQ(adopted(*manager), 3u);

    const auto manual = std::dynamic_pointer_cast<ManualStrategy>(manager->get_strategy(ManualStrategy::ID));
    ASSERT_NE(manual, nullptr);
    const auto held = manual->get_subscriptions();
    ASSERT_EQ(held.size(), 2u);
    EXPECT_EQ(held[0].broker_id, BrokerId::CME);
    EXPECT_EQ(held[0].instruments, std::vector<std::string>{"ZNZ6"});
    EXPECT_EQ(held[1].broker_id, BrokerId::ICE);
    EXPECT_EQ(held[1].instruments, (std::vector<std::string>{"BRNF7", "BRNG7"}));
}

TEST_F(MdSubscriptionTest, ManualListingNothingAdoptsNothing) {
    auto manager = manual_manager(md_qs_, nlohmann::json::object(), nlohmann::json::object());
    const auto event = definition(BrokerId::CME, "ZNZ6");
    manager->process_event(&event);

    EXPECT_EQ(adopted(*manager), 0u);
    const auto manual = std::dynamic_pointer_cast<ManualStrategy>(manager->get_strategy(ManualStrategy::ID));
    ASSERT_NE(manual, nullptr);
    EXPECT_TRUE(manual->get_subscriptions().empty());
}

// A broker built with market data off reads no request, and waiting on it would hold its
// requests until stop() gave up on it. Only an enabled connector is waited on.
TEST_F(MdSubscriptionTest, ABrokerWithMarketDataOffIsNotWaitedOn) {
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"*"}));
    define_channel(manager_, BrokerId::CME, 0, 2048);   // more than CME's queue holds, left unread
    BrokerStatusRegistry::instance().set_md_enabled(BrokerId::CME, false);
    manager_.start();

    // Well within md_request_stall_timeout_ms, the 5 s stop() waits on an enabled connector.
    const auto stopping = std::chrono::steady_clock::now();
    manager_.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stopping, std::chrono::seconds(2))
        << "stop() waited on a broker with market data off";
}

// A broker this engine did not build has no request queue: what is asked of it is held, and
// sent nowhere.
TEST_F(MdSubscriptionTest, ABrokerThisEngineDidNotBuildIsAskedForNothing) {
    MdRequestQueues cme_only(65536);
    Connector cme(cme_only.get(BrokerId::CME), BrokerId::CME);
    auto manager = manual_manager(cme_only, nlohmann::json::object());
    for (const auto broker : {BrokerId::SIMULATED, BrokerId::CME}) {
        const auto event = definition(broker, "ZNZ6");
        manager->process_event(&event);
    }
    manager->start();
    EXPECT_TRUE(wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(5),
                           [&] { return cme.subscribed_count() == 1; }));
    manager->stop();
    cme.stop();
    EXPECT_EQ(cme.unsubscribed, std::multiset<std::string>{"ZNZ6"});
}

namespace {

// MANUAL's parameters as the Web UI sends them, through the bus, to be handled as the worker does.
void update_manual(StrategyManager& manager, const nlohmann::json& params) {
    const std::string text = params.dump();
    std::vector<uint8_t> record(StrategyConfigEvent::size_for(text.size()));
    auto* event = new (record.data()) StrategyConfigEvent();
    event->set_strategy(ManualStrategy::ID);
    event->set_config(text);
    manager.process_event(event);
}

} // namespace

// A list MANUAL was given at runtime adopted only what brokers defined after it, and they had
// defined theirs already: no ladder until a restart. A new list now adopts what was defined
// before it. An update without one empties it, as it does the file it is saved to.
TEST_F(MdSubscriptionTest, AListManualIsGivenAtRuntimeAdoptsWhatWasDefinedBefore) {
    auto manager = manual_manager(md_qs_, nlohmann::json::object(), manual_lists({"ZNZ6"}));
    for (const auto* instrument : {"ZNZ6", "ZBZ6", "ZBH7"}) {
        const auto event = definition(BrokerId::CME, instrument);
        manager->process_event(&event);
    }
    EXPECT_EQ(adopted(*manager), 1u);

    update_manual(*manager, manual_lists({"ZB*"}));
    EXPECT_EQ(adopted(*manager), 3u) << "the ZB contracts defined already were not adopted";

    const auto later = definition(BrokerId::CME, "ZBM7");
    manager->process_event(&later);
    EXPECT_EQ(adopted(*manager), 4u);
    update_manual(*manager, {{"default_qty", 2}});
    const auto unlisted = definition(BrokerId::CME, "ZBU7");
    manager->process_event(&unlisted);
    EXPECT_EQ(adopted(*manager), 4u) << "an update without \"instruments\" left a list";

    const auto manual = std::dynamic_pointer_cast<ManualStrategy>(manager->get_strategy(ManualStrategy::ID));
    ASSERT_NE(manual, nullptr);
    const auto held = manual->get_subscriptions();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(std::multiset<std::string>(held[0].instruments.begin(), held[0].instruments.end()),
              (std::multiset<std::string>{"ZNZ6", "ZBZ6", "ZBH7", "ZBM7"}))
        << "what is held stays held";
}

// MANUAL adopted what a definition naming no broker defined, under UNKNOWN: the ladder listed it
// with broker 0, which takes no order, and its subscription had no queue to go to.
TEST_F(MdSubscriptionTest, ManualAdoptsNothingOfNoBroker) {
    add_strategy(std::make_shared<ManualStrategy>(), manual_lists({"*"}));
    const auto event = definition(BrokerId::UNKNOWN, "ZNZ6");
    manager_.process_event(&event);
    EXPECT_EQ(adopted(manager_), 0u);
}

// The cursor a connector reports is the registry's, not its queue's: one a connector of an earlier
// queue left let the manager take a new queue for read up to there, and write over what its own
// connector had not read. A new queue has had nothing read.
TEST(MdRequestQueuesTest, ANewQueueHasHadNothingRead) {
    auto& registry = BrokerStatusRegistry::instance();
    registry.set_md_request_cursor(BrokerId::ICE, 12345);
    MdRequestQueues queues(4096);
    queues.get(BrokerId::ICE);
    EXPECT_EQ(registry.md_request_cursor(BrokerId::ICE), BrokerStatus::NO_MD_REQUEST_CURSOR);
}
