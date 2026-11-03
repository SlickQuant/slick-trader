#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// Servicing a broker's sockets from its own worker thread, with no receive threads and
// no hand-off between threads: whatever arrives goes from the socket to the protocol
// session in the loop iteration that finds it. Templated on the socket types, a
// caller-driven ("polled") TCP client and multicast receiver, so this header depends on
// no socket library, and a test can drive it with fakes.
namespace slick::trader {

// The connection a protocol session talks over.
class ByteTransport {
public:
    virtual ~ByteTransport() = default;
    virtual void send(const uint8_t* data, size_t size) = 0;
    // Drops the connection at the link's next service(), never from inside the
    // session's own callback.
    virtual void close() = 0;
};

// TCP delivers a stream: a message may end in a later read. Hands the consumer what has
// arrived and keeps the tail it did not take.
class StreamAssembler {
public:
    // `consume(const uint8_t*, size_t) -> size_t` returns how many bytes it took.
    template <typename Consume>
    void feed(const uint8_t* data, size_t size, Consume&& consume) {
        if (partial_.empty()) {
            const size_t consumed = consume(data, size);
            if (consumed < size) {
                partial_.assign(data + consumed, data + size);
            }
        } else {
            partial_.insert(partial_.end(), data, data + size);
            const size_t consumed = consume(partial_.data(), partial_.size());
            partial_.erase(partial_.begin(), partial_.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
    }
    void clear() noexcept { partial_.clear(); }
    [[nodiscard]] size_t pending() const noexcept { return partial_.size(); }

private:
    std::vector<uint8_t> partial_;
};

// One TCP connection serviced by its owner's worker: made, read, handed to the session,
// and made again after `reconnect_ns` when it drops.
//
// `Client` has the polled TCP client's interface: State {Disconnected, Connecting,
// Connected}, state(), poll(), connect(), disconnect(), is_connected(),
// receive(std::span<uint8_t>) and send(std::span<const uint8_t>).
//
// The session given to service() has on_connected(now), on_data(data, size, now) ->
// bytes consumed, on_timer(now), on_disconnected() and ready_to_connect(now) -> bool.
template <typename Client>
class PolledLink : public ByteTransport {
public:
    PolledLink(std::unique_ptr<Client> client, uint64_t reconnect_ns)
        : client_(std::move(client))
        , reconnect_ns_(reconnect_ns)
    {}

    void send(const uint8_t* data, size_t size) override {
        // Straight from the session's buffer to the socket. A failed send has closed the
        // connection: the session is told on the next service().
        client_->send(std::span<const uint8_t>(data, size));
    }
    void close() override { close_requested_ = true; }

    // Connects, reads and runs the session's timer; true when anything was read.
    template <typename Session>
    bool service(Session& session, uint64_t now_ns, std::span<uint8_t> buffer, int max_reads = 16) {
        using State = typename Client::State;
        bool worked = false;
        if (client_->state() == State::Connecting) {
            client_->poll();   // made, failed or timed out
        }
        if (client_->state() == State::Connected) {
            if (!connected_) {
                connected_ = true;
                close_requested_ = false;
                assembler_.clear();
                session.on_connected(now_ns);
            }
            for (int i = 0; i < max_reads && !close_requested_; ++i) {
                const std::ptrdiff_t received = client_->receive(buffer);
                if (received <= 0) {
                    break;   // nothing waiting, or the connection is gone (handled below)
                }
                worked = true;
                assembler_.feed(buffer.data(), static_cast<size_t>(received), [&](const uint8_t* data, size_t size) {
                    return session.on_data(data, size, now_ns);
                });
            }
            if (client_->is_connected() && !close_requested_) {
                session.on_timer(now_ns);
            }
        }
        if (close_requested_) {
            // The session asked for the connection to be dropped: done here rather than
            // from inside its callbacks.
            close_requested_ = false;
            client_->disconnect();
        }
        if (client_->state() == State::Disconnected) {
            if (connected_) {
                connected_ = false;
                assembler_.clear();
                next_connect_ns_ = now_ns + reconnect_ns_;
                session.on_disconnected();
            }
            if (enabled_ && now_ns >= next_connect_ns_ && session.ready_to_connect(now_ns)) {
                next_connect_ns_ = now_ns + reconnect_ns_;
                client_->connect();
            }
        }
        return worked;
    }

    // Drops the connection without telling the session: the owner is shutting down.
    void disconnect() {
        client_->disconnect();
        connected_ = false;
        assembler_.clear();
    }

    // A link that is not enabled is not connected again once it drops.
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    // Makes the next service() connect at once, whatever the reconnect pacing says.
    void connect_now() noexcept { next_connect_ns_ = 0; }

    // The session has been told it is connected.
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    [[nodiscard]] Client& client() noexcept { return *client_; }

private:
    std::unique_ptr<Client> client_;
    uint64_t reconnect_ns_;
    uint64_t next_connect_ns_ = 0;
    StreamAssembler assembler_;
    bool connected_ = false;
    bool close_requested_ = false;
    bool enabled_ = true;
};

// A multicast line needed only some of the time (a snapshot or definition loop). Its
// socket stays joined but is not read while unwanted: leaving and rejoining a group
// takes longer than a gap is allowed to. What piled up unread is stale by the time it is
// wanted again, and is thrown away first.
//
// `Receiver` has receive(std::span<uint8_t>) -> bytes, 0 or less for none.
template <typename Receiver>
class GatedLine {
public:
    GatedLine() = default;
    explicit GatedLine(std::unique_ptr<Receiver> receiver) : receiver_(std::move(receiver)) {}

    // While `wanted`, applies up to `max` packets with `apply(const uint8_t*, size_t)`;
    // returns how many. What piled up while it was not wanted is thrown away first and
    // counted in discarded().
    template <typename Apply>
    uint32_t read(bool wanted, std::span<uint8_t> buffer, uint32_t max, Apply&& apply) {
        discarded_ = 0;
        if (!receiver_) {
            return 0;
        }
        if (wanted && !reading_) {
            while (receiver_->receive(buffer) > 0) {
                ++discarded_;
            }
        }
        reading_ = wanted;
        if (!wanted) {
            return 0;
        }
        uint32_t applied = 0;
        for (; applied < max; ++applied) {
            const std::ptrdiff_t size = receiver_->receive(buffer);
            if (size <= 0) {
                break;
            }
            apply(buffer.data(), static_cast<size_t>(size));
        }
        return applied;
    }

    void reset(std::unique_ptr<Receiver> receiver) {
        receiver_ = std::move(receiver);
        reading_ = false;
    }
    [[nodiscard]] Receiver* receiver() noexcept { return receiver_.get(); }
    // Packets the last read() threw away: they show the group is live, though unused.
    [[nodiscard]] uint32_t discarded() const noexcept { return discarded_; }

private:
    std::unique_ptr<Receiver> receiver_;
    uint32_t discarded_ = 0;
    bool reading_ = false;
};

} // namespace slick::trader
