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
// The clients are raw sockets, so the tests need POSIX and are skipped elsewhere.

#include "strategy_manager_fixture.hpp"
#include <gtest/gtest.h>
#include <slick-trader/web/web_server.hpp>

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>

using namespace slick::trader;
using namespace std::chrono_literals;

namespace {

constexpr auto STOP_LIMIT = 5s;
// How long a stop() that missed its limit is given once its clients are gone.
constexpr auto HANG_LIMIT = 10s;

/// Asks the kernel for an unused port. On failure the test fails fatally and `port` stays 0,
/// which a server would take to mean "any port" and which no client could find.
void free_port(uint16_t& port) {
    port = 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int named = bound == 0 ? ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) : -1;
    ::close(fd);   // before the asserts, which return
    ASSERT_EQ(bound, 0);
    ASSERT_EQ(named, 0);
    port = ntohs(addr.sin_port);
}

/// A client on a raw socket. It sends what a browser sends and reads the server's
/// WebSocket frames back.
class RawClient {
public:
    explicit RawClient(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
        }
    }
    ~RawClient() { close(); }
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    [[nodiscard]] bool connected() const { return fd_ >= 0; }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /// The WebSocket opening handshake. Frames the server sends straight after its answer
    /// (the welcome message) stay buffered for read_until_closed().
    [[nodiscard]] bool upgrade() {
        return exchange("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
                        "HTTP/1.1 101");
    }

    /// A request that leaves the connection open and idle, like a browser's kept-alive fetch.
    [[nodiscard]] bool keep_alive_request() {
        return exchange("GET /api/health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
                        "HTTP/1.1 200");
    }

    struct Outcome {
        bool close_frame = false;   ///< the server sent a WebSocket close frame
        uint16_t code = 0;          ///< its status code
    };

    /// Reads frames until a close frame, the end of the stream, or `timeout`.
    Outcome read_until_closed(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (const auto outcome = parse_frames()) {
                return *outcome;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= 0ms || !fill(left)) {
                return {};
            }
        }
    }

    /// Answers a close frame as a browser does: with one of its own (clients mask theirs).
    /// Two header bytes, the 4 byte mask key, then the 2 byte status code xored with it.
    void answer_close(uint16_t code) {
        const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        const uint8_t frame[8] = {0x88, 0x82, mask[0], mask[1], mask[2], mask[3],
                                  static_cast<uint8_t>((code >> 8) ^ mask[0]),
                                  static_cast<uint8_t>((code & 0xFF) ^ mask[1])};
        ::send(fd_, frame, sizeof(frame), MSG_NOSIGNAL);
    }

private:
    bool exchange(const std::string& request, const std::string& expected_status) {
        if (fd_ < 0 || ::send(fd_, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
            return false;
        }
        while (buffer_.find("\r\n\r\n") == std::string::npos) {
            if (!fill(2000ms)) {
                return false;
            }
        }
        const auto head_end = buffer_.find("\r\n\r\n") + 4;
        const bool ok = buffer_.compare(0, expected_status.size(), expected_status) == 0;
        buffer_.erase(0, head_end);
        return ok;
    }

    bool fill(std::chrono::milliseconds timeout) {
        if (fd_ < 0) {
            return false;
        }
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0) {
            return false;
        }
        char chunk[4096];
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;   // the server closed the connection
        }
        buffer_.append(chunk, static_cast<size_t>(n));
        return true;
    }

    /// Returns the first close frame read so far and consumes the frames before it, or
    /// nothing if there is none yet. The server's frames are not masked.
    std::optional<Outcome> parse_frames() {
        auto byte = [this](size_t i) { return static_cast<uint8_t>(buffer_[i]); };
        while (buffer_.size() >= 2) {
            size_t header = 2;
            uint64_t length = byte(1) & 0x7F;
            if (length == 126) {
                if (buffer_.size() < 4) return std::nullopt;
                length = (static_cast<uint64_t>(byte(2)) << 8) | byte(3);
                header = 4;
            } else if (length == 127) {
                if (buffer_.size() < 10) return std::nullopt;
                length = 0;
                for (size_t i = 2; i < 10; ++i) length = (length << 8) | byte(i);
                header = 10;
            }
            if (buffer_.size() < header + length) return std::nullopt;
            if ((byte(0) & 0x0F) == 0x8) {
                const uint16_t code = length >= 2 ? static_cast<uint16_t>((byte(header) << 8) | byte(header + 1)) : 0;
                return Outcome{true, code};
            }
            buffer_.erase(0, header + length);
        }
        return std::nullopt;
    }

    int fd_ = -1;
    std::string buffer_;
};

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
        blocker_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(blocker_, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        socklen_t len = sizeof(addr);
        ASSERT_EQ(::bind(blocker_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(blocker_, 1), 0);
        ASSERT_EQ(::getsockname(blocker_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);
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
    GTEST_SKIP() << "the clients are raw POSIX sockets";
}

#endif
