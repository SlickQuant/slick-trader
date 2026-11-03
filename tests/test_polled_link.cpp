// Servicing a broker's sockets from its worker: a TCP link connected, read, reassembled
// and reconnected (PolledLink, StreamAssembler), a multicast line read only while it is
// wanted (GatedLine), and the kill switch's loop. Driven with fake sockets.

#include <gtest/gtest.h>
#include <slick-trader/brokers/common/kill_switch.hpp>
#include <slick-trader/brokers/common/polled_link.hpp>
#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

// The polled TCP client's interface, scripted.
class FakeClient {
public:
    enum class State : uint8_t { Disconnected, Connecting, Connected };

    State state() const noexcept { return state_; }
    State poll() { return state_ = reachable ? State::Connected : State::Disconnected; }
    bool connect() {
        ++connects;
        state_ = State::Connecting;
        return true;
    }
    void disconnect() noexcept { state_ = State::Disconnected; }
    bool is_connected() const noexcept { return state_ == State::Connected; }
    std::ptrdiff_t receive(std::span<uint8_t> buffer) {
        if (state_ != State::Connected || inbound.empty()) {
            return 0;
        }
        const std::string chunk = inbound.front();
        inbound.pop_front();
        std::memcpy(buffer.data(), chunk.data(), chunk.size());
        return static_cast<std::ptrdiff_t>(chunk.size());
    }
    bool send(std::span<const uint8_t> data) {
        sent.emplace_back(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }

    bool reachable = true;
    int connects = 0;
    std::deque<std::string> inbound;
    std::vector<std::string> sent;

private:
    State state_ = State::Disconnected;
};

// A session whose messages are lines ending in '\n'.
struct LineSession {
    void on_connected(uint64_t) { ++connected; }
    size_t on_data(const uint8_t* data, size_t size, uint64_t) {
        size_t used = 0;
        for (size_t i = 0; i < size; ++i) {
            if (data[i] == '\n') {
                lines.emplace_back(reinterpret_cast<const char*>(data + used), i - used);
                used = i + 1;
            }
        }
        return used;
    }
    void on_timer(uint64_t) { ++timers; }
    void on_disconnected() { ++disconnected; }
    bool ready_to_connect(uint64_t) const { return ready; }

    int connected = 0;
    int disconnected = 0;
    int timers = 0;
    bool ready = true;
    std::vector<std::string> lines;
};

constexpr uint64_t RECONNECT = 100;

} // namespace

TEST(PolledLinkTest, ConnectsReadsWholeMessagesAndKeepsTheTail) {
    auto client = std::make_unique<FakeClient>();
    FakeClient& c = *client;
    PolledLink<FakeClient> link(std::move(client), RECONNECT);
    LineSession session;
    std::vector<uint8_t> buffer(64);

    link.service(session, 0, buffer);   // starts connecting
    EXPECT_EQ(c.connects, 1);
    c.inbound = {"hel", "lo\nwor", "ld\n"};
    EXPECT_TRUE(link.service(session, 1, buffer));
    EXPECT_EQ(session.connected, 1);
    EXPECT_EQ(session.lines, (std::vector<std::string>{"hello", "world"}));
    EXPECT_GT(session.timers, 0);

    const uint8_t out[] = {'x', '\n'};
    link.send(out, sizeof(out));
    EXPECT_EQ(c.sent, (std::vector<std::string>{"x\n"}));
}

TEST(PolledLinkTest, ADroppedConnectionIsReportedOnceAndMadeAgainAfterThePause) {
    auto client = std::make_unique<FakeClient>();
    FakeClient& c = *client;
    PolledLink<FakeClient> link(std::move(client), RECONNECT);
    LineSession session;
    std::vector<uint8_t> buffer(64);
    link.service(session, 0, buffer);
    link.service(session, 1, buffer);
    ASSERT_TRUE(link.connected());

    c.inbound = {"partial"};
    link.service(session, 2, buffer);
    c.disconnect();
    link.service(session, 3, buffer);
    EXPECT_EQ(session.disconnected, 1);
    EXPECT_EQ(c.connects, 1) << "not straight away";
    link.service(session, 3 + RECONNECT, buffer);
    EXPECT_EQ(c.connects, 2);
    c.inbound = {"line\n"};
    link.service(session, 4 + RECONNECT, buffer);
    EXPECT_EQ(session.lines, (std::vector<std::string>{"line"})) << "the old connection's tail is gone";
}

TEST(PolledLinkTest, CloseFromTheSessionIsDoneOnTheNextServiceAndASessionCanHoldOffConnecting) {
    auto client = std::make_unique<FakeClient>();
    FakeClient& c = *client;
    PolledLink<FakeClient> link(std::move(client), RECONNECT);
    LineSession session;
    session.ready = false;
    std::vector<uint8_t> buffer(64);
    link.service(session, 0, buffer);
    EXPECT_EQ(c.connects, 0) << "the session is not ready";
    session.ready = true;
    link.service(session, 1, buffer);
    link.service(session, 2, buffer);
    ASSERT_TRUE(link.connected());

    link.close();   // as a session does from inside a callback
    EXPECT_TRUE(c.is_connected());
    link.service(session, 3, buffer);
    EXPECT_FALSE(c.is_connected());
    EXPECT_EQ(session.disconnected, 1);
}

TEST(PolledLinkTest, AGatedLineThrowsAwayWhatPiledUpWhileItWasNotWanted) {
    struct Receiver {
        std::ptrdiff_t receive(std::span<uint8_t> buffer) {
            if (packets.empty()) {
                return 0;
            }
            buffer[0] = packets.front();
            packets.pop_front();
            return 1;
        }
        std::deque<uint8_t> packets;
    };
    auto receiver = std::make_unique<Receiver>();
    Receiver& r = *receiver;
    GatedLine<Receiver> line(std::move(receiver));
    std::vector<uint8_t> buffer(8);
    std::vector<uint8_t> applied;
    auto apply = [&](const uint8_t* data, size_t) { applied.push_back(data[0]); };

    r.packets = {1, 2};
    EXPECT_EQ(line.read(false, buffer, 10, apply), 0u);
    EXPECT_EQ(line.discarded(), 0u) << "not wanted: left waiting";
    r.packets.push_back(3);
    EXPECT_EQ(line.read(true, buffer, 10, apply), 0u) << "stale: 1, 2 and 3 waited unread";
    EXPECT_EQ(line.discarded(), 3u);
    r.packets = {4, 5, 6};
    EXPECT_EQ(line.read(true, buffer, 2, apply), 2u) << "at most max a pass";
    EXPECT_EQ(line.discarded(), 0u) << "only the pass that starts reading throws away";
    EXPECT_EQ(applied, (std::vector<uint8_t>{4, 5}));
}

TEST(KillSwitchTest, StopsWhenNothingIsLeftOrTheTimeIsUp) {
    int steps = 0;
    auto left = run_kill_switch(
        "test", 3, 1'000'000'000ULL, [](uint64_t) { return true; },
        [&](uint64_t) {
            ++steps;
            return KillSwitchCount{static_cast<size_t>(3 - std::min(steps, 3)), 0};
        },
        "nothing else");
    EXPECT_EQ(left.waiting, 0u);
    EXPECT_EQ(steps, 3);

    left = run_kill_switch(
        "test", 2, 1, [](uint64_t) { return false; }, [](uint64_t) { return KillSwitchCount{1, 1}; }, "nothing");
    EXPECT_EQ(left.waiting, 1u) << "the deadline passed";
    EXPECT_EQ(left.unreachable, 1u);

    left = run_kill_switch(
        "test", 2, 0, [](uint64_t) { return false; }, [](uint64_t) { return KillSwitchCount{}; }, "nothing");
    EXPECT_EQ(left.waiting, 2u) << "cancel_on_stop_ms 0: nothing is cancelled";
    EXPECT_EQ(run_kill_switch("test", 0, 1, [](uint64_t) { return false; },
                              [](uint64_t) { return KillSwitchCount{5, 5}; }, "nothing")
                  .waiting,
              0u)
        << "nothing working: nothing to do";
}
