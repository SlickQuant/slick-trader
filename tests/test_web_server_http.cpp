// The WebServer's HTTP answers, read off a raw socket. Unlike test_web_server_shutdown.cpp, these
// run on Windows too.

#include "strategy_manager_fixture.hpp"
#include <gtest/gtest.h>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/ladder_subscription_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/strategy/manual_strategy.hpp>
#include <slick-trader/web/web_server.hpp>

#include "raw_client.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace slick::trader;
using test::free_port;
using test::RawClient;

namespace {

class WebServerHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::create_directories(static_dir_);
        std::ofstream(static_dir_ / "index.html") << "<html></html>";
        // MANUAL, with risk limits, and a broker this engine built: no connector in this build makes
        // one. All before the server starts, as the engine has it: the server reads them from its
        // own thread.
        env_.add_strategy(manual_, nlohmann::json{{"instruments", {"Z*"}}});
        RiskLimits limits;
        limits.max_position_size = to_qty_t(1.0);
        env_.risk_.set_limits(ManualStrategy::ID, limits);
        BrokerStatusRegistry::instance().set_name(BROKER, "Alpaca");
        producer_ = env_.mux_.add_producer(env_.mux_.producer_count(), 1u << 16, 4096);
        ASSERT_NO_FATAL_FAILURE(free_port(port_));
        ASSERT_TRUE(server_.initialize(nlohmann::json{{"web_server_port", port_},
                                                      {"web_server_static_path", static_dir_.string()},
                                                      {"web_server_ladder_release_ms", release_ms_}}));
        server_.start();
        ASSERT_TRUE(wait_until_listening());
    }

    void TearDown() override {
        server_.stop();
        auto& brokers = BrokerStatusRegistry::instance();
        brokers.set_order_connected(BROKER, false);
        brokers.set_order_enabled(BROKER, false);
        brokers.set_md_connected(BROKER, false);
        brokers.set_md_enabled(BROKER, false);
        brokers.set_name(BROKER, "");
        std::error_code ignored;
        std::filesystem::remove_all(static_dir_, ignored);
    }

    bool wait_until_listening() const {
        for (int i = 0; i < 100; ++i) {
            if (RawClient(port_).connected()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    static constexpr BrokerId BROKER = BrokerId::ALPACA;
    static std::string order(double quantity) {
        return R"({"instrument":"ZNZ6","broker_id":)" + std::to_string(static_cast<int>(BROKER)) +
               R"(,"side":"buy","type":"limit","price":110.5,"quantity":)" + std::to_string(quantity) + "}";
    }

    static constexpr std::string_view SUBSCRIBE_ZNZ6 = R"({"type":"subscribe","channel":"level2","instrument":"ZNZ6"})";
    static constexpr std::string_view UNSUBSCRIBE_ZNZ6 = R"({"type":"unsubscribe","channel":"level2","instrument":"ZNZ6"})";
    static constexpr std::string_view SUBSCRIBE_INSTRUMENTS = R"({"type":"subscribe","channel":"instruments"})";

    // The next message `browser` reads, or null.
    static nlohmann::json next(RawClient& browser) {
        const auto text = browser.read_text(std::chrono::seconds(5));
        return text ? nlohmann::json::parse(*text) : nlohmann::json();
    }

    // Whether nothing reached `browser` in between: a ping's pong is the next message.
    static bool nothing_came(RawClient& browser) {
        if (!browser.send_text(R"({"type":"ping"})")) {
            return false;
        }
        const auto message = next(browser);
        return message.is_object() && message.value("type", "") == "pong";
    }

    // The next hold or release the server put on the bus within `timeout`, read from `cursor_`.
    std::optional<LadderSubscriptionEvent> next_ladder_event(std::chrono::milliseconds timeout) {
        for (const auto deadline = std::chrono::steady_clock::now() + timeout;
             std::chrono::steady_clock::now() < deadline;) {
            if (auto record = env_.mux_.read(cursor_)) {
                const auto* event = reinterpret_cast<const Event*>(record.data);
                if (event->type == EventType::LADDER_SUBSCRIPTION) {
                    return *reinterpret_cast<const LadderSubscriptionEvent*>(event);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return std::nullopt;
    }

    // A snapshot of ZNZ6 from BROKER, `per_side` levels deep.
    void publish_znz6(uint16_t per_side) {
        EventSerializer::publish<Level2UpdateEvent>(
            *producer_, sizeof(Level2UpdateEvent) + 2 * per_side * sizeof(PriceLevel), [&](Level2UpdateEvent& event) {
                event.broker_id = BROKER;
                event.set_instrument("ZNZ6");
                event.is_snapshot = true;
                event.num_levels = static_cast<uint16_t>(2 * per_side);
                for (uint16_t i = 0; i < per_side; ++i) {
                    event.levels[2 * i] = PriceLevel(to_price_t(110.0 - i / 64.0), to_qty_t(1000.0 + i), Side::BUY);
                    event.levels[2 * i + 1] = PriceLevel(to_price_t(110.5 + i / 64.0), to_qty_t(1000.0 + i), Side::SELL);
                }
            });
    }

    // How many levels `browser` is sent as it subscribes to ZNZ6, or nothing.
    static std::optional<size_t> subscribed_levels(RawClient& browser) {
        if (!browser.send_text(SUBSCRIBE_ZNZ6)) {
            return std::nullopt;
        }
        while (const auto text = browser.read_text(std::chrono::seconds(2))) {
            const auto message = nlohmann::json::parse(*text);
            if (message["type"] == "level2") {
                return message["levels"].size();
            }
        }
        return std::nullopt;
    }

    // Subscribes `browser` to ZNZ6 until it is sent `levels`, once the server has read what was
    // published.
    static bool wait_for_levels(RawClient& browser, size_t levels) {
        for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             std::chrono::steady_clock::now() < deadline;) {
            if (subscribed_levels(browser) == levels) {
                return true;
            }
        }
        return false;
    }

    uint16_t port_ = 0;
    uint64_t release_ms_ = 50;   // web_server_ladder_release_ms
    std::filesystem::path static_dir_ = std::filesystem::temp_directory_path() /
        ("slick-trader-http-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    test::StrategyManagerFixture env_;
    // A broker's end of the bus the server reads. Added before the server starts, as producers are.
    std::shared_ptr<slick::stream_buffer_multiplexer::producer_buffer> producer_;
    uint64_t cursor_ = env_.mux_.initial_reading_index();   // where next_ladder_event() reads the bus
    std::shared_ptr<ManualStrategy> manual_ = std::make_shared<ManualStrategy>();
    WebServer server_{"", env_.manager_, env_.positions_, env_.pnl_, env_.risk_, env_.mux_};
};

} // namespace

// Static files went out with max-age=3600, so a page reloaded after the engine was rebuilt ran
// the scripts the browser had kept.
TEST_F(WebServerHttpTest, StaticFilesAreNotKept) {
    RawClient browser(port_);
    ASSERT_TRUE(browser.get("/index.html", "HTTP/1.1 200")) << browser.head();
    EXPECT_NE(browser.head().find("Cache-Control: no-cache"), std::string::npos) << browser.head();
    EXPECT_EQ(browser.head().find("max-age"), std::string::npos) << browser.head();
}

// An API error went out as 200 OK: send_json_response() wrote the CORS headers before the
// status, and uWS sends 200 with the first header written ahead of one. The dashboard took an
// order the server refused for one it had sent. Then its status line had no reason phrase.
TEST_F(WebServerHttpTest, AnApiErrorKeepsItsStatus) {
    RawClient client(port_);
    EXPECT_TRUE(client.post("/api/orders", R"({"instrument":"ZNZ6","broker_id":0,"side":"buy","quantity":1})",
                            "HTTP/1.1 400 Bad Request\r\n"))
        << client.head();
}

// broker_id was read into a byte, so an order for broker 256 + n went to broker n.
TEST_F(WebServerHttpTest, ABrokerIdPastAByteNamesNoBroker) {
    RawClient client(port_);
    const std::string order = R"({"instrument":"ZNZ6","broker_id":)" + std::to_string(256 + static_cast<int>(BROKER)) +
                              R"(,"side":"buy","quantity":1})";
    EXPECT_TRUE(client.post("/api/orders", order, "HTTP/1.1 400 Bad Request\r\n")) << client.head();
}

// An order for a broker that cannot take one now is turned down with why, as unavailable.
TEST_F(WebServerHttpTest, AnOrderForABrokerThatTakesNoneNowIs503) {
    RawClient client(port_);
    EXPECT_TRUE(client.post("/api/orders", order(1), "HTTP/1.1 503 Service Unavailable\r\n")) << client.head();
}

// An order MANUAL's risk limits turned down was answered 202 "Order submitted successfully", and
// the dashboard showed it sent. It is now refused, with the reason.
TEST_F(WebServerHttpTest, AnOrderTurnedDownBeforeItLeavesIs422) {
    auto& brokers = BrokerStatusRegistry::instance();
    brokers.set_order_enabled(BROKER, true);
    brokers.set_order_connected(BROKER, true);

    RawClient refused(port_);
    EXPECT_TRUE(refused.post("/api/orders", order(5), "HTTP/1.1 422 Unprocessable Content\r\n")) << refused.head();
    RawClient sent(port_);
    EXPECT_TRUE(sent.post("/api/orders", order(1), "HTTP/1.1 202 Accepted\r\n")) << sent.head();
}

// Each time MANUAL listed instruments, every dashboard refetched every strategy, each with every
// instrument it held: a channel's worth, again and again. The ladder's list now comes over the
// WebSocket to a page that asks, all of it and then what MANUAL lists since, each with its broker
// tick and smallest order, and /api/strategies leaves it out. It names MANUAL "builtin", as it is no plugin.
TEST_F(WebServerHttpTest, TheLadderListComesWholeWhenAskedThenWhatIsNew) {
    // From this thread, as the StrategyManager's worker lists while the server reads.
    ASSERT_TRUE(manual_->add_listing(BrokerId::CME, "ZNZ6", to_price_t(0.015625), to_qty_t(1)));
    const int cme = static_cast<int>(BrokerId::CME);
    const int ice = static_cast<int>(BrokerId::ICE);

    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    EXPECT_EQ(next(browser)["type"], "welcome");
    ASSERT_TRUE(browser.send_text(SUBSCRIBE_INSTRUMENTS));
    EXPECT_EQ(next(browser), (nlohmann::json{{"type", "instruments"}, {"instruments", {{"ZNZ6", {cme, 0.015625, 1}}}}}));

    ASSERT_TRUE(manual_->add_listing(BrokerId::ICE, "ZBZ6", to_price_t(0.01), to_qty_t(0.5)));
    // ZNZ6 may come again, listed before the server last told its dashboards: one that asked
    // since gets it twice, and the ladder takes it once.
    nlohmann::json added = nlohmann::json::object();
    while (!added.contains("ZBZ6")) {
        auto message = next(browser);
        ASSERT_EQ(message["type"], "instruments_added") << message.dump();
        added.update(message["instruments"]);
    }
    EXPECT_EQ(added["ZBZ6"], (nlohmann::json{ice, 0.01, 0.5}));
    added.erase("ZNZ6");
    EXPECT_EQ(added.size(), 1u) << added.dump();

    RawClient client(port_);
    ASSERT_TRUE(client.get("/api/strategies", "HTTP/1.1 200 OK\r\n")) << client.head();
    const auto strategies = nlohmann::json::parse(client.body());
    ASSERT_EQ(strategies.size(), 1u) << client.body();
    EXPECT_EQ(strategies[0].value("type", ""), "builtin");
    EXPECT_FALSE(strategies[0].contains("instruments"));
    EXPECT_FALSE(strategies[0].contains("instrument_brokers"));
}


// The list runs to megabytes with MANUAL on "*", and went to every window as it connected: an
// Orders or Fills window has no ladder. It goes only to a page that asks.
TEST_F(WebServerHttpTest, AClientNotAskingGetsNoInstruments) {
    ASSERT_TRUE(manual_->add_listing(BrokerId::CME, "ZNZ6"));
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    EXPECT_EQ(next(browser)["type"], "welcome");
    EXPECT_TRUE(nothing_came(browser));

    ASSERT_TRUE(manual_->add_listing(BrokerId::CME, "ZBZ6"));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // past the 250 ms instruments_added wait
    EXPECT_TRUE(nothing_came(browser));
}

// MANUAL subscribed every instrument it took on, with no ladder showing any. The first ladder on
// one of MANUAL's has it held at its broker now, and the last letting go has it released, after
// web_server_ladder_release_ms. A second ladder, and the first one's going, ask for nothing.
TEST_F(WebServerHttpTest, ALadderOnAListedInstrumentHoldsItAndReleasesItAfterTheGrace) {
    ASSERT_TRUE(manual_->add_listing(BrokerId::CME, "ZNZ6"));
    RawClient first(port_);
    RawClient second(port_);
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());
    ASSERT_EQ(subscribed_levels(first), 0u);

    const auto held = next_ladder_event(std::chrono::seconds(5));
    ASSERT_TRUE(held) << "no hold was asked for";
    EXPECT_TRUE(held->subscribe);
    EXPECT_EQ(held->broker_id, BrokerId::CME);
    EXPECT_EQ(std::string_view(held->instrument), "ZNZ6");

    ASSERT_EQ(subscribed_levels(second), 0u);
    first.close();
    EXPECT_FALSE(next_ladder_event(std::chrono::milliseconds(300))) << "released while a ladder showed it";

    ASSERT_TRUE(second.send_text(UNSUBSCRIBE_ZNZ6));
    const auto released = next_ladder_event(std::chrono::seconds(5));
    ASSERT_TRUE(released) << "never released";
    EXPECT_FALSE(released->subscribe);
    EXPECT_EQ(std::string_view(released->instrument), "ZNZ6");
    EXPECT_FALSE(next_ladder_event(std::chrono::milliseconds(300)));
}

// A strategy's instrument, and one nobody lists, stream without a hold.
TEST_F(WebServerHttpTest, AnInstrumentMANUALDoesNotListIsWatchedAlone) {
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    ASSERT_EQ(subscribed_levels(browser), 0u);
    EXPECT_FALSE(next_ladder_event(std::chrono::milliseconds(300)));
}

namespace {

// Holds for longer than the test runs.
class WebServerLongGraceTest : public WebServerHttpTest {
protected:
    WebServerLongGraceTest() { release_ms_ = 60'000; }
};

} // namespace

// A ladder reloaded, or switched back, within the grace finds its instrument still held: nothing is
// asked of its broker again.
TEST_F(WebServerLongGraceTest, AReturnWithinTheGraceKeepsTheHold) {
    ASSERT_TRUE(manual_->add_listing(BrokerId::CME, "ZNZ6"));
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    ASSERT_EQ(subscribed_levels(browser), 0u);
    ASSERT_TRUE(next_ladder_event(std::chrono::seconds(5)));

    ASSERT_TRUE(browser.send_text(UNSUBSCRIBE_ZNZ6));
    ASSERT_EQ(subscribed_levels(browser), 0u);
    browser.close();
    RawClient reloaded(port_);
    ASSERT_TRUE(reloaded.upgrade());
    ASSERT_EQ(subscribed_levels(reloaded), 0u);
    EXPECT_FALSE(next_ladder_event(std::chrono::milliseconds(300))) << "held or released again";
}

// The dashboard shows the engine's version, read from /api/health.
TEST_F(WebServerHttpTest, HealthNamesTheVersion) {
    RawClient client(port_);
    ASSERT_TRUE(client.get("/api/health", "HTTP/1.1 200")) << client.head();
    EXPECT_EQ(nlohmann::json::parse(client.body()),
              (nlohmann::json{{"status", "ok"}, {"version", SLICK_TRADER_VERSION}}));
}

// A Fills window opened late had nothing to show: fills went out over the WebSocket alone, and named
// no strategy. The server keeps the last ones for /api/fills, each with the seq it was sent with.
TEST_F(WebServerHttpTest, AFillIsListedAndBroadcastWithItsSeq) {
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    EXPECT_EQ(next(browser)["type"], "welcome");

    EventSerializer::publish<OrderAckEvent>(*producer_, [](OrderAckEvent& ack) {
        ack.broker_id = BROKER;
        ack.set_instrument("ZNZ6");
        ack.set_client_order_id("MANUAL_1");
        ack.set_order_id("OID1");
        ack.side = Side::BUY;
        ack.price = to_price_t(110.5);
        ack.quantity = to_qty_t(2.0);
    });
    EventSerializer::publish<OrderFillEvent>(*producer_, [](OrderFillEvent& fill) {
        fill.broker_id = BROKER;
        fill.set_instrument("ZNZ6");
        fill.set_client_order_id("MANUAL_1");
        fill.set_order_id("OID1");
        fill.set_fill_id("F1");
        fill.side = Side::BUY;
        fill.fill_price = to_price_t(110.5);
        fill.fill_quantity = to_qty_t(1.0);
        fill.remaining_quantity = to_qty_t(1.0);
    });

    nlohmann::json sent = nlohmann::json::object();
    for (int i = 0; i < 10 && sent.is_object() && sent.value("event", "") != "fill"; ++i) {
        sent = next(browser);
    }
    ASSERT_TRUE(sent.is_object());
    ASSERT_EQ(sent.value("event", ""), "fill") << sent.dump();
    EXPECT_EQ(sent["seq"], 1);
    EXPECT_EQ(sent["strategy_id"], "MANUAL");
    EXPECT_EQ(sent["client_order_id"], "MANUAL_1");
    EXPECT_EQ(sent["instrument"], "ZNZ6");
    EXPECT_EQ(sent["fill_id"], "F1");
    EXPECT_EQ(sent["fill_qty"], 1.0);

    RawClient client(port_);
    ASSERT_TRUE(client.get("/api/fills", "HTTP/1.1 200 OK\r\n")) << client.head();
    EXPECT_EQ(nlohmann::json::parse(client.body()), nlohmann::json::array({sent}));
}

// uWS drops a message for a client too far behind, and what send() said went unread: that
// client's ladder stayed short of the update for good, or its list short of an instrument. It is
// closed now, and reconnects to both whole.
//
// It was closed only once the server had answered every ask uWS had read in, each answer then
// dropped: with a deep book, 20 s and more past the drop.
TEST_F(WebServerHttpTest, AClientTooFarBehindIsClosed) {
    publish_znz6(1024);   // each subscribe is answered with 2048 levels, about 40 KB
    RawClient reader(port_);
    ASSERT_TRUE(reader.upgrade());
    ASSERT_TRUE(wait_for_levels(reader, 2048));

    // A browser that asks and asks and reads nothing, not even to see the close: reading took in
    // what the server held for it, so on a slow run it never got 16 MB behind.
    RawClient slow(port_, 4096);
    ASSERT_TRUE(slow.upgrade());
    constexpr auto CLOSED_WITHIN = std::chrono::seconds(15);
    const auto start = std::chrono::steady_clock::now();
    bool closed = false;
    while (!closed && std::chrono::steady_clock::now() - start < CLOSED_WITHIN) {
        closed = !slow.send_text(SUBSCRIBE_ZNZ6);
    }
    // A send blocked while the server is busy can fail past the limit.
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(closed) << "kept open, missing what uWS dropped";
    EXPECT_LT(took, CLOSED_WITHIN) << "closed only after answering what it asked past the drop";
}

// The list a dashboard gets on connect runs to megabytes with MANUAL on "*", past uWS's 64 KB
// default. A browser on a slow link still taking it in had its next message dropped and was
// closed, and got the whole list again as it reconnected, over and over. A browser megabytes
// behind is kept now. Here they are snapshots: Windows takes one big message into the socket
// whole, where a slow link would not.
TEST_F(WebServerHttpTest, ABrowserMegabytesBehindIsKept) {
    publish_znz6(256);
    RawClient reader(port_);
    ASSERT_TRUE(reader.upgrade());
    ASSERT_TRUE(wait_for_levels(reader, 512));

    RawClient slow(port_, 4096);
    ASSERT_TRUE(slow.upgrade());
    constexpr int ASKED = 300;   // about 3 MB of snapshots
    for (int i = 0; i < ASKED; ++i) {
        ASSERT_TRUE(slow.send_text(SUBSCRIBE_ZNZ6));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // answered, and none read yet
    for (int snapshots = 0; snapshots < ASKED;) {
        const auto text = slow.read_text(std::chrono::seconds(5));
        ASSERT_TRUE(text) << "closed after " << snapshots << " snapshot(s)";
        snapshots += text->starts_with(R"({"type":"level2")") ? 1 : 0;
    }
}

// A book whose broker's feed was down went to a new subscriber as a fresh snapshot, though it was
// as the feed left it. It goes out empty until the feed is back.
TEST_F(WebServerHttpTest, ABookWhoseFeedIsDownIsSentEmpty) {
    auto& brokers = BrokerStatusRegistry::instance();
    brokers.set_md_enabled(BROKER, true);
    brokers.set_md_connected(BROKER, true);
    publish_znz6(2);
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    ASSERT_TRUE(wait_for_levels(browser, 4));

    brokers.set_md_connected(BROKER, false);
    EXPECT_EQ(subscribed_levels(browser), 0u) << "sent the book as the feed left it";
    brokers.set_md_connected(BROKER, true);
    EXPECT_EQ(subscribed_levels(browser), 4u);
}

// A request's body reached its handler as the last chunk uWS read, alone: one that came in more
// than one, as a long MANUAL "instruments" list does, was parsed from its middle and refused.
TEST_F(WebServerHttpTest, ABodyInMoreThanOneChunkIsReadWhole) {
    auto& brokers = BrokerStatusRegistry::instance();
    brokers.set_order_enabled(BROKER, true);
    brokers.set_order_connected(BROKER, true);

    RawClient client(port_);
    EXPECT_TRUE(client.post_in_two("/api/orders", order(1), "HTTP/1.1 202 Accepted\r\n"))
        << client.head() << client.body();
}

namespace {

// Subscribes ZBZ6 at CME, and says whether that was asked of it off the thread that made it.
class ThreadBoundStrategy : public StrategyBase {
public:
    ThreadBoundStrategy() : StrategyBase("ThreadBound", "1.0.0") {}

    std::vector<SubscriptionRequest> get_subscriptions() const override {
        if (std::this_thread::get_id() != owner_) {
            asked_off_thread.store(true, std::memory_order_relaxed);
        }
        return {SubscriptionRequest(BrokerId::CME, SubscriptionType::LEVEL2, {"ZBZ6"})};
    }

    mutable std::atomic<bool> asked_off_thread{false};

private:
    const std::thread::id owner_ = std::this_thread::get_id();
};

class WebServerWithAStrategyTest : public WebServerHttpTest {
protected:
    void SetUp() override {
        env_.add_strategy(strategy_);
        WebServerHttpTest::SetUp();
    }

    std::shared_ptr<ThreadBoundStrategy> strategy_ = std::make_shared<ThreadBoundStrategy>();
};

} // namespace

namespace {

// Subscribes ZNZ6 at CME as `type`.
class ZNZ6Strategy : public StrategyBase {
public:
    explicit ZNZ6Strategy(SubscriptionType type) : StrategyBase("ZNZ6", "1.0.0"), type_(type) {}

    std::vector<SubscriptionRequest> get_subscriptions() const override {
        return {SubscriptionRequest(BrokerId::CME, type_, {"ZNZ6"})};
    }

private:
    SubscriptionType type_;
};

template <SubscriptionType TYPE>
class WebServerWithAZNZ6StrategyTest : public WebServerHttpTest {
protected:
    void SetUp() override {
        env_.add_strategy(std::make_shared<ZNZ6Strategy>(TYPE));
        WebServerHttpTest::SetUp();
    }
};

using WebServerWithAQuotingStrategyTest = WebServerWithAZNZ6StrategyTest<SubscriptionType::QUOTE>;
using WebServerWithAnOrderBookStrategyTest = WebServerWithAZNZ6StrategyTest<SubscriptionType::ORDER_BOOK>;

} // namespace

// A ladder on any strategy's instrument was taken for streamed, so one a strategy only quoted got
// no book: quotes and trades stream no level 2. MANUAL holds it now, as it does any other.
TEST_F(WebServerWithAQuotingStrategyTest, ALadderHoldsTheBookOfAnInstrumentAStrategyOnlyQuotes) {
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    ASSERT_EQ(subscribed_levels(browser), 0u);

    const auto held = next_ladder_event(std::chrono::seconds(5));
    ASSERT_TRUE(held) << "no hold was asked for";
    EXPECT_TRUE(held->subscribe);
    EXPECT_EQ(held->broker_id, BrokerId::CME);
    EXPECT_EQ(std::string_view(held->instrument), "ZNZ6");
}

// A ladder on what a strategy streams as ORDER_BOOK had MANUAL hold its LEVEL2 as well. Coinbase
// serves both on one channel, so the hold's release unsubscribed the running strategy's book.
// ORDER_BOOK feeds a ladder as LEVEL2 does: nothing is held.
TEST_F(WebServerWithAnOrderBookStrategyTest, ALadderOnWhatAStrategyStreamsAsAnOrderBookHoldsNothing) {
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    ASSERT_EQ(subscribed_levels(browser), 0u);
    EXPECT_FALSE(next_ladder_event(std::chrono::milliseconds(300))) << "MANUAL held a book a strategy streams";
}

// A dashboard connecting had the server call each strategy's get_subscriptions() on its own
// thread, while the StrategyManager's could be writing the same state in the strategy's
// on_config(). What they subscribe is taken once, before either thread runs.
TEST_F(WebServerWithAStrategyTest, AStrategysInstrumentsAreListedWithNoCallOffItsThread) {
    RawClient browser(port_);
    ASSERT_TRUE(browser.upgrade());
    EXPECT_EQ(next(browser)["type"], "welcome");
    ASSERT_TRUE(browser.send_text(SUBSCRIBE_INSTRUMENTS));
    EXPECT_EQ(next(browser), (nlohmann::json{{"type", "instruments"},
                                             {"instruments", {{"ZBZ6", {static_cast<int>(BrokerId::CME), 0, 0}}}}}));
    EXPECT_FALSE(strategy_->asked_off_thread.load(std::memory_order_relaxed));

    // A ladder on it streams what the strategy subscribed: MANUAL holds nothing for it.
    ASSERT_TRUE(browser.send_text(R"({"type":"subscribe","channel":"level2","instrument":"ZBZ6"})"));
    EXPECT_FALSE(next_ladder_event(std::chrono::milliseconds(300)));
}
