#pragma once

// A client on a raw socket, for tests of the WebServer: it sends what a browser sends and reads
// the answers back, WebSocket frames included. On POSIX sockets and Winsock alike.
//
// Include it after the WebServer's headers: on Windows it brings in winsock2.h.

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "Ws2_32.lib")
    #endif
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace slick::trader::test {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t NO_SOCKET = INVALID_SOCKET;
inline constexpr int SEND_FLAGS = 0;
inline void close_socket(socket_t s) { ::closesocket(s); }
inline int poll_socket(pollfd& p, int timeout_ms) { return ::WSAPoll(&p, 1, timeout_ms); }
// Winsock is started once for the process, and stays.
inline void start_sockets() {
    static const bool started = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
#else
using socket_t = int;
inline constexpr socket_t NO_SOCKET = -1;
inline constexpr int SEND_FLAGS = MSG_NOSIGNAL;
inline void close_socket(socket_t s) { ::close(s); }
inline int poll_socket(pollfd& p, int timeout_ms) { return ::poll(&p, 1, timeout_ms); }
inline void start_sockets() {}
#endif

/// Asks the system for an unused port. On failure the test fails fatally and `port` stays 0,
/// which a server would take to mean "any port" and which no client could find.
inline void free_port(uint16_t& port) {
    start_sockets();
    port = 0;
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(fd, NO_SOCKET);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int named = bound == 0 ? ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) : -1;
    close_socket(fd);   // before the asserts, which return
    ASSERT_EQ(bound, 0);
    ASSERT_EQ(named, 0);
    port = ntohs(addr.sin_port);
}

class RawClient {
public:
    /// With `receive_buffer`, the socket takes in at most about that many bytes the test has not
    /// read, as a browser on a slow link would: what the server sends past it waits there.
    explicit RawClient(uint16_t port, int receive_buffer = 0) {
        start_sockets();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ != NO_SOCKET && receive_buffer > 0) {
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer),
                         sizeof(receive_buffer));
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd_ != NO_SOCKET && ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
        }
    }
    ~RawClient() { close(); }
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    [[nodiscard]] bool connected() const { return fd_ != NO_SOCKET; }

    void close() {
        if (fd_ != NO_SOCKET) {
            close_socket(fd_);
            fd_ = NO_SOCKET;
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

    /// GETs `path` and checks the status line starts with `expected_status`.
    [[nodiscard]] bool get(const std::string& path, const std::string& expected_status) {
        return exchange("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n", expected_status);
    }

    /// POSTs `body` as JSON and checks the status line starts with `expected_status`.
    [[nodiscard]] bool post(const std::string& path, const std::string& body, const std::string& expected_status) {
        return exchange("POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body,
                        expected_status);
    }

    /// As post(), with the body sent in two writes a pause apart, as a long one arrives: the server
    /// reads it in more than one chunk.
    [[nodiscard]] bool post_in_two(const std::string& path, const std::string& body, const std::string& expected_status) {
        const size_t half = body.size() / 2;
        const std::string first = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body.substr(0, half);
        if (fd_ == NO_SOCKET ||
            ::send(fd_, first.data(), static_cast<int>(first.size()), SEND_FLAGS) != static_cast<int>(first.size())) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return exchange(body.substr(half), expected_status);
    }

    /// The status line and headers of the last answer, and its body.
    [[nodiscard]] const std::string& head() const { return head_; }
    [[nodiscard]] const std::string& body() const { return body_; }

    struct Outcome {
        bool close_frame = false;   ///< the server sent a WebSocket close frame
        uint16_t code = 0;          ///< its status code
    };

    /// Reads frames until a close frame, the end of the stream, or `timeout`.
    Outcome read_until_closed(std::chrono::milliseconds timeout) {
        return read_until(timeout, [this] { return parse_frames(); }).value_or(Outcome{});
    }

    /// The next text frame's payload, the frames before it skipped, or nothing by `timeout`.
    std::optional<std::string> read_text(std::chrono::milliseconds timeout) {
        return read_until(timeout, [this]() -> std::optional<std::string> {
            while (const auto frame = buffered_frame()) {
                std::string payload = buffer_.substr(frame->header, frame->length);
                buffer_.erase(0, frame->header + frame->length);
                if (frame->opcode == 0x1) {
                    return payload;
                }
            }
            return std::nullopt;
        });
    }

    /// Answers a close frame as a browser does: with one of its own, the status code its payload.
    void answer_close(uint16_t code) {
        const char payload[2] = {static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
        send_frame(0x8, std::string_view(payload, sizeof(payload)));
    }

    /// Sends `text` in a text frame. False once the connection is gone.
    bool send_text(std::string_view text) { return send_frame(0x1, text); }

    /// Reads and drops what the server sends until it closes the connection: false if it has not
    /// by `timeout`.
    bool wait_closed(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            buffer_.clear();
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds(0)) {
                return false;
            }
            if (!fill(left)) {
                return std::chrono::steady_clock::now() < deadline;
            }
        }
    }

private:
    /// One frame with its `opcode`, masked as a client's must be: the header, the 4 byte mask
    /// key, then the payload xored with it. Payloads under 64 KB.
    bool send_frame(uint8_t opcode, std::string_view payload) {
        const char mask[4] = {0x12, 0x34, 0x56, 0x78};
        std::string frame(1, static_cast<char>(0x80 | opcode));
        if (payload.size() < 126) {
            frame += static_cast<char>(0x80 | payload.size());
        } else {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(payload.size() >> 8);
            frame += static_cast<char>(payload.size() & 0xFF);
        }
        frame.append(mask, sizeof(mask));
        for (size_t i = 0; i < payload.size(); ++i) {
            frame += static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        return fd_ != NO_SOCKET &&
               ::send(fd_, frame.data(), static_cast<int>(frame.size()), SEND_FLAGS) == static_cast<int>(frame.size());
    }

    bool exchange(const std::string& request, const std::string& expected_status) {
        if (fd_ == NO_SOCKET ||
            ::send(fd_, request.data(), static_cast<int>(request.size()), SEND_FLAGS) != static_cast<int>(request.size())) {
            return false;
        }
        while (buffer_.find("\r\n\r\n") == std::string::npos) {
            if (!fill(std::chrono::milliseconds(2000))) {
                return false;
            }
        }
        const auto head_end = buffer_.find("\r\n\r\n") + 4;
        head_ = buffer_.substr(0, head_end);
        buffer_.erase(0, head_end);
        // A 101 has none: what follows it is frames.
        body_.clear();
        constexpr std::string_view LENGTH = "Content-Length: ";
        if (const auto at = head_.find(LENGTH); at != std::string::npos) {
            const size_t length = std::stoul(head_.substr(at + LENGTH.size()));
            while (buffer_.size() < length) {
                if (!fill(std::chrono::milliseconds(2000))) {
                    return false;
                }
            }
            body_ = buffer_.substr(0, length);
            buffer_.erase(0, length);
        }
        return head_.compare(0, expected_status.size(), expected_status) == 0;
    }

    // Calls `take()` on what is buffered, and reads more, until it returns something or
    // `timeout` passes.
    template <typename Take>
    auto read_until(std::chrono::milliseconds timeout, Take take) -> decltype(take()) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (auto taken = take()) {
                return taken;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds(0) || !fill(left)) {
                return {};
            }
        }
    }

    bool fill(std::chrono::milliseconds timeout) {
        if (fd_ == NO_SOCKET) {
            return false;
        }
        pollfd p{};
        p.fd = fd_;
        p.events = POLLIN;
        if (poll_socket(p, static_cast<int>(timeout.count())) <= 0) {
            return false;
        }
        char chunk[4096];
        const auto n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;   // the server closed the connection
        }
        buffer_.append(chunk, static_cast<size_t>(n));
        return true;
    }

    struct Frame {
        uint8_t opcode;
        size_t header;   ///< bytes before the payload
        size_t length;   ///< of the payload
    };

    /// The first frame buffered, once all of it is. The server's frames are not masked.
    std::optional<Frame> buffered_frame() const {
        auto byte = [this](size_t i) { return static_cast<uint8_t>(buffer_[i]); };
        if (buffer_.size() < 2) return std::nullopt;
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
        return Frame{static_cast<uint8_t>(byte(0) & 0x0F), header, static_cast<size_t>(length)};
    }

    /// Returns the first close frame read so far and consumes the frames before it, or
    /// nothing if there is none yet.
    std::optional<Outcome> parse_frames() {
        while (const auto frame = buffered_frame()) {
            if (frame->opcode == 0x8) {
                const auto byte = [this](size_t i) { return static_cast<uint8_t>(buffer_[i]); };
                const uint16_t code = frame->length >= 2
                    ? static_cast<uint16_t>((byte(frame->header) << 8) | byte(frame->header + 1)) : 0;
                return Outcome{true, code};
            }
            buffer_.erase(0, frame->header + frame->length);
        }
        return std::nullopt;
    }

    socket_t fd_ = NO_SOCKET;
    std::string buffer_;
    std::string head_;
    std::string body_;
};

} // namespace slick::trader::test
