// WebServer::stop() has to return while a client still has a connection open.
//
// The bug: stop() closed the listen socket from the calling thread, which uWS doesn't
// allow, then joined the worker. The event loop runs as long as anything is open on it, so
// a dashboard tab left open kept the join, and the whole shutdown, waiting until the tab
// closed. Pressing Ctrl+C again did nothing.
//
// The fix defers the shutdown onto the loop's own thread. Every WebSocket gets a 1001
// (going away) close frame and App::close() drops whatever else is open.
//
// The file also covers two related cases. stop() must not use the worker's loop after the
// worker has exited and freed it. And a worker whose listen failed leaves a thread that
// stop(), start() and the destructor still have to join.
//
// They are written for POSIX sockets, and skipped elsewhere. test_web_server_http.cpp is not.

#include "strategy_manager_fixture.hpp"
#include <gtest/gtest.h>
#include <slick-trader/web/web_server.hpp>

#ifndef _WIN32

#include "raw_client.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <string>
#include <thread>

using namespace slick::trader;
using namespace std::chrono_literals;
using test::free_port;
using test::RawClient;

namespace {

constexpr auto STOP_LIMIT = 5s;
// How long a stop() that missed its limit is given once its clients are gone.
constexpr auto HANG_LIMIT = 10s;

/// Runs stop() on its own thread, so a stop that never returns fails a check instead of
/// hanging the test. `release` does what the bug was waiting for (closing the clients) and
/// runs on the way out if stop() hasn't returned by then.
class BackgroundStop {
public:
    BackgroundStop(WebServer& server, std::function<void()> release)
        : release_(std::move(release))
        , done_(std::async(std::launch::async, [&server] { server.stop(); }))
    {}
    ~BackgroundStop() {
        if (done_.wait_for(0ms) != std::future_status::ready) {
            release_();
        }
        // A stop() that still hasn't returned is stuck in a join nothing can end. Waiting
        // for it, here or in the server's destructor, would hang the run for good, so exit
        // with the reason instead.
        if (done_.wait_for(HANG_LIMIT) != std::future_status::ready) {
            std::fputs("FATAL: WebServer::stop() never returned; its worker's event loop was left running\n", stderr);
            std::_Exit(2);
        }
    }
    BackgroundStop(const BackgroundStop&) = delete;
    BackgroundStop& operator=(const BackgroundStop&) = delete;

    [[nodiscard]] bool returned_within(std::chrono::milliseconds limit) {
        return done_.wait_for(limit) == std::future_status::ready;
    }

private:
    std::function<void()> release_;
    std::future<void> done_;
};

/// A server that has not been started yet, and the port the tests look for it on.
class WebServerTest : public ::testing::Test {
protected:
    bool accepts_connections() const { return RawClient(port_).connected(); }

    bool wait_until_listening() const {
        for (int i = 0; i < 100; ++i) {
            if (accepts_connections()) {
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    uint16_t port_ = 0;
    test::StrategyManagerFixture env_;
    WebServer server_{"", env_.manager_, env_.positions_, env_.pnl_, env_.risk_, env_.mux_};
};

/// A server that is up and listening.
class WebServerShutdownTest : public WebServerTest {
protected:
    void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(free_port(port_));
        ASSERT_TRUE(server_.initialize(nlohmann::json{{"web_server_port", port_}}));
        server_.start();
        ASSERT_TRUE(wait_until_listening());
    }
};

/// A server whose port is taken by someone else, so its listen fails and its worker gives up
/// on its own. Nobody calls stop() for that, which leaves the worker thread to be joined.
class WebServerListenFailureTest : public WebServerTest {
protected:
    void SetUp() override {
        // Hold the port the way uSockets asks for it: [::] with IPV6_V6ONLY off, or 0.0.0.0
        // without IPv6. An IPv4 blocker isn't enough. Linux refuses the server's dual-stack
        // bind next to it, but macOS lets it through and the listen succeeds.
        sockaddr_in6 addr{};
        socklen_t len = sizeof(addr);
        blocker_ = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (blocker_ >= 0) {
            const int off = 0;
            ASSERT_EQ(::setsockopt(blocker_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)), 0);
            addr.sin6_family = AF_INET6;
            addr.sin6_addr = in6addr_any;
        } else {
            blocker_ = ::socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(blocker_, 0);
            auto& in4 = reinterpret_cast<sockaddr_in&>(addr);
            in4.sin_family = AF_INET;
            in4.sin_addr.s_addr = htonl(INADDR_ANY);
            len = sizeof(sockaddr_in);
        }
        ASSERT_EQ(::bind(blocker_, reinterpret_cast<sockaddr*>(&addr), len), 0);
        ASSERT_EQ(::listen(blocker_, 1), 0);
        ASSERT_EQ(::getsockname(blocker_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        // sin_port and sin6_port sit at the same offset.
        port_ = ntohs(addr.sin6_port);
        ASSERT_TRUE(server_.initialize(nlohmann::json{{"web_server_port", port_}}));
    }

    void TearDown() override { release_port(); }

    void release_port() {
        if (blocker_ >= 0) {
            ::close(blocker_);
            blocker_ = -1;
        }
    }

    /// The worker clears running_ when its listen fails.
    static bool wait_for_worker_to_give_up(const WebServer& server) {
        for (int i = 0; i < 500; ++i) {
            if (!server.is_running()) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    int blocker_ = -1;
};

} // namespace

TEST_F(WebServerShutdownTest, StopReturnsWhileAClientHoldsAWebSocketOpen) {
    // The reported hang: a dashboard tab that never answers and never closes.
    RawClient tab(port_);
    ASSERT_TRUE(tab.upgrade());

    BackgroundStop stop(server_, [&] { tab.close(); });
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT)) << "stop() waited for the client to close its WebSocket";
    EXPECT_FALSE(server_.is_running());
}

TEST_F(WebServerShutdownTest, EveryWebSocketClientIsToldTheServerIsGoingAway) {
    RawClient first(port_);
    RawClient second(port_);
    ASSERT_TRUE(first.upgrade());
    ASSERT_TRUE(second.upgrade());

    BackgroundStop stop(server_, [&] {
        first.close();
        second.close();
    });
    for (auto* client : {&first, &second}) {
        const auto outcome = client->read_until_closed(STOP_LIMIT);
        ASSERT_TRUE(outcome.close_frame) << "a close frame, not just a dropped connection";
        EXPECT_EQ(outcome.code, 1001) << "going away";
        client->answer_close(outcome.code);
    }
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT));
}

TEST_F(WebServerShutdownTest, StopReturnsWhileAnIdleKeepAliveConnectionIsOpen) {
    // A browser keeps the connection behind a fetch() open. uWS would drop it after its
    // own idle timeout, which is longer than a shutdown should take.
    RawClient browser(port_);
    ASSERT_TRUE(browser.keep_alive_request());

    BackgroundStop stop(server_, [&] { browser.close(); });
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT)) << "stop() waited for the idle connection to time out";
}

TEST_F(WebServerShutdownTest, StopReturnsWithAWebSocketAndAKeepAliveConnectionOpenTogether) {
    // What a dashboard tab actually holds open.
    RawClient socket(port_);
    RawClient fetch(port_);
    ASSERT_TRUE(socket.upgrade());
    ASSERT_TRUE(fetch.keep_alive_request());

    BackgroundStop stop(server_, [&] {
        socket.close();
        fetch.close();
    });
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT));
}

TEST_F(WebServerShutdownTest, StopReturnsWithNobodyConnected) {
    BackgroundStop stop(server_, [] {});
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT));
}

TEST_F(WebServerShutdownTest, StopClosesTheListenSocket) {
    EXPECT_TRUE(accepts_connections());
    server_.stop();
    EXPECT_FALSE(accepts_connections()) << "the port is still being listened on";
}

TEST_F(WebServerShutdownTest, StoppingTwiceIsHarmless) {
    RawClient tab(port_);
    ASSERT_TRUE(tab.upgrade());

    {
        BackgroundStop stop(server_, [&] { tab.close(); });
        ASSERT_TRUE(stop.returned_within(STOP_LIMIT));
    }
    server_.stop();   // already stopped: returns at once
    EXPECT_FALSE(server_.is_running());
}

TEST_F(WebServerShutdownTest, AStoppedServerCanBeStartedAndStoppedAgain) {
    server_.stop();
    server_.start();
    ASSERT_TRUE(wait_until_listening());

    RawClient tab(port_);
    ASSERT_TRUE(tab.upgrade());
    BackgroundStop stop(server_, [&] { tab.close(); });
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT));
}

TEST_F(WebServerShutdownTest, StopRightAfterStartNeverMissesTheShutdown) {
    // stop() can run before the worker has its loop up, so it has no loop to hand the
    // shutdown to and the worker has to notice on its own. A stop that missed it would join
    // a loop that runs forever, and BackgroundStop would then exit the process.
    server_.stop();
    for (int i = 0; i < 20; ++i) {
        server_.start();
        BackgroundStop stop(server_, [] {});
        ASSERT_TRUE(stop.returned_within(STOP_LIMIT)) << "iteration " << i;
        ASSERT_FALSE(server_.is_running()) << "iteration " << i;
    }
}

TEST_F(WebServerShutdownTest, StopAtEveryPointOfTheWorkersStartAndExitNeverTouchesAFreedLoop) {
    // stop() hands the shutdown to the worker's loop with defer(), and that loop is freed when
    // the worker thread exits. A stop() that picked the loop up just before the worker retired
    // it, and was then slow, used a freed loop. The window is a few instructions wide, so this
    // sweeps the delay between start() and stop() across the worker's whole start and exit.
    // A plain run rarely lands in the window. Under a sanitizer or valgrind, a hit is
    // reported at once.
    server_.stop();
    for (int i = 0; i < 300; ++i) {
        server_.start();
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((i % 100) * 3);
        while (std::chrono::steady_clock::now() < until) {
        }
        BackgroundStop stop(server_, [] {});
        ASSERT_TRUE(stop.returned_within(STOP_LIMIT)) << "iteration " << i;
        ASSERT_FALSE(server_.is_running()) << "iteration " << i;
    }
}

TEST_F(WebServerListenFailureTest, StopJoinsAWorkerWhoseListenFailed) {
    // running_ is already false here, but the worker thread is still there to be joined. Left
    // joinable, the thread's destructor terminates the process, in stop() or in ~WebServer.
    server_.start();
    ASSERT_TRUE(wait_for_worker_to_give_up(server_));

    BackgroundStop stop(server_, [] {});
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT));
    EXPECT_FALSE(server_.is_running());
    // ~WebServer runs stop() again, with nothing left to join.
}

TEST_F(WebServerListenFailureTest, DestructorJoinsAWorkerWhoseListenFailed) {
    // Nobody calls stop(): the destructor has to deal with the worker on its own.
    WebServer failing{"", env_.manager_, env_.positions_, env_.pnl_, env_.risk_, env_.mux_};
    ASSERT_TRUE(failing.initialize(nlohmann::json{{"web_server_port", port_}}));
    failing.start();
    ASSERT_TRUE(wait_for_worker_to_give_up(failing));
}

TEST_F(WebServerListenFailureTest, StartJoinsAWorkerWhoseListenFailed) {
    // start() on a server whose last worker gave up must not assign over its thread.
    server_.start();
    ASSERT_TRUE(wait_for_worker_to_give_up(server_));

    release_port();
    server_.start();
    ASSERT_TRUE(wait_until_listening());
    EXPECT_TRUE(server_.is_running());

    BackgroundStop stop(server_, [] {});
    EXPECT_TRUE(stop.returned_within(STOP_LIMIT));
    EXPECT_FALSE(accepts_connections());
}

#else // _WIN32

TEST(WebServerShutdownTest, SkippedWithoutPosixSockets) {
    GTEST_SKIP() << "written for POSIX sockets";
}

#endif
