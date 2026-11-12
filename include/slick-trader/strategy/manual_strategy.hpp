#pragma once

#include <slick-trader/strategy/strategy_base.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slick::trader {

// Lightweight strategy that owns all orders placed manually through the Web UI.
// Always registered at startup. It lists the instruments brokers define for the Web UI's
// ladders to pick from, and subscribes one's level 2 book only while a ladder shows it: the
// WebServer holds and releases them over the bus. In MANUAL.config.json (any <name>.config.json
// naming it), parameter "instruments" narrows the list: symbols, or prefixes ending in '*'
// ("ZN*"). With no such parameter it lists everything defined, with [] nothing.
class ManualStrategy : public StrategyBase {
public:
    static constexpr const char* ID = "MANUAL";
    static constexpr uint32_t CHUNK_SIZE = 256;
    static constexpr uint32_t MAX_INSTRUMENTS = CHUNK_SIZE * 256;
    // What it subscribes of each instrument a ladder holds.
    static constexpr SubscriptionType SUBSCRIPTION = SubscriptionType::LEVEL2;

    ManualStrategy() : StrategyBase(ID, "1.0.0") {}

    // The level 2 book of what ladders hold, one request per broker. Each is subscribed as it is
    // held, not at start(). Read where the StrategyManager thread is not running: by
    // add_strategy(), when nothing is held, and by stop() after the worker has joined, which
    // unsubscribes them and has release_all() let go of them.
    std::vector<SubscriptionRequest> get_subscriptions() const override {
        std::vector<SubscriptionRequest> subs;
        for (size_t b = 0; b < held_.size(); ++b) {
            if (held_[b].empty()) {
                continue;
            }
            auto& sub = subs.emplace_back(static_cast<BrokerId>(b), SUBSCRIPTION, std::vector<std::string>{});
            sub.instruments.assign(held_[b].begin(), held_[b].end());
        }
        return subs;
    }

    // Calls `f(broker, instrument, tick_size, min_qty)` (or without min_qty, for an `f` that takes
    // three) for each instrument listed from the `from`th on, in
    // the order listed, and returns how many are listed: passed back as `from`, it hands over only
    // what is new. Safe from any thread: the WebServer reads it while the StrategyManager lists.
    template <typename F>
    uint32_t for_each_listed(uint32_t from, F&& f) const {
        const uint32_t count = count_.load(std::memory_order_acquire);
        for (uint32_t i = from; i < count; ++i) {
            const Slot& slot = chunks_[i / CHUNK_SIZE][i % CHUNK_SIZE];
            if constexpr (std::is_invocable_v<F&, BrokerId, std::string_view, price_t, qty_t>) {
                f(slot.broker, std::string_view(slot.instrument), slot.tick_size, slot.min_qty);
            } else {
                f(slot.broker, std::string_view(slot.instrument), slot.tick_size);
            }
        }
        return count;
    }

    uint32_t listed_count() const noexcept { return count_.load(std::memory_order_acquire); }

    // Lists an instrument a broker defined. False when the config does not take it, when listed
    // already, when full, or when no broker is named: one of UNKNOWN takes no order and has no
    // request queue. StrategyManager thread only: it is the one writer.
    bool add_listing(BrokerId broker, std::string_view instrument, price_t tick_size = 0, qty_t min_qty = 0) {
        const auto b = static_cast<size_t>(broker);
        // Listed already is the common case, as a venue replays its definitions: found with no
        // string built.
        if (broker == BrokerId::UNKNOWN || b >= listed_.size() || !lists(instrument) ||
            listed_[b].contains(instrument)) {
            return false;
        }
        const uint32_t n = count_.load(std::memory_order_relaxed);
        if (n == MAX_INSTRUMENTS) [[unlikely]] {
            // Once: it stays full, and a venue's worth past it would each log a line.
            if (!full_) {
                full_ = true;
                LOG_WARN("Strategy {}: listing the maximum of {} instruments, so {} and any it is offered after "
                         "are not listed",
                         ID, MAX_INSTRUMENTS, instrument);
            }
            return false;
        }
        listed_[b].emplace(instrument);
        auto& chunk = chunks_[n / CHUNK_SIZE];
        if (!chunk) {
            chunk = std::make_unique<Slot[]>(CHUNK_SIZE);
        }
        Slot& slot = chunk[n % CHUNK_SIZE];
        slot.broker = broker;
        slot.tick_size = tick_size;
        slot.min_qty = min_qty;
        const size_t len = std::min(instrument.size(), sizeof(slot.instrument) - 1);
        std::memcpy(slot.instrument, instrument.data(), len);
        slot.instrument[len] = '\0';
        count_.store(n + 1, std::memory_order_release);
        return true;
    }

    // Whether the config takes `instrument`: by its symbol or a prefix, or all with no list.
    bool lists(std::string_view instrument) const {
        return all_ || symbols_.contains(instrument) ||
               std::ranges::any_of(prefixes_, [&](const std::string& prefix) { return instrument.starts_with(prefix); });
    }

    // A ladder shows `instrument`: true when that makes it held. Only one listed at `broker` can
    // be, or one `subscribed` there by a strategy, which ladders list too. release() lets go, true
    // when it was held. StrategyManager thread only.
    bool hold(BrokerId broker, std::string_view instrument, bool subscribed = false) {
        const auto b = static_cast<size_t>(broker);
        return b < held_.size() && (subscribed || listed_[b].contains(instrument)) &&
               held_[b].emplace(instrument).second;
    }
    bool release(BrokerId broker, std::string_view instrument) {
        const auto b = static_cast<size_t>(broker);
        if (b >= held_.size()) {
            return false;
        }
        const auto it = held_[b].find(instrument);
        if (it == held_[b].end()) {
            return false;
        }
        held_[b].erase(it);
        return true;
    }
    void release_all() {
        for (auto& held : held_) {
            held.clear();
        }
    }

protected:
    bool on_initialize() override { return true; }

    // At load, and on a parameter update from the Web UI, on the StrategyManager thread, which
    // then lists the instruments brokers defined already that the new list takes. An update
    // replaces the list, as it does every parameter, here and in the file it is saved to: one
    // without "instruments" takes everything. What is listed stays listed.
    void on_config(const nlohmann::json& params) override {
        symbols_.clear();
        prefixes_.clear();
        const auto it = params.find("instruments");
        all_ = it == params.end() || !it->is_array();
        if (all_) {
            LOG_INFO("Strategy {}: lists every instrument brokers define", ID);
            return;
        }
        for (const auto& entry : *it) {
            if (!entry.is_string()) {
                continue;
            }
            auto symbol = entry.get<std::string>();
            if (symbol.ends_with('*')) {
                symbol.pop_back();
                prefixes_.push_back(std::move(symbol));
            } else {
                symbols_.insert(std::move(symbol));
            }
        }
        LOG_INFO("Strategy {}: lists {} symbol(s) and {} prefix(es) as brokers define them", ID, symbols_.size(),
                 prefixes_.size());
    }

private:
    struct Slot {
        BrokerId broker;
        price_t tick_size;   // 0 when its broker did not say
        qty_t min_qty;       // 0 when its broker did not say
        char instrument[MAX_INSTRUMENT_LENGTH];
    };

    static constexpr size_t BROKER_COUNT = static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_);

    // Append-only, so readers take no lock: a slot is written before the count that covers
    // it is released, and never changes after. Chunks grow memory only with what is listed.
    std::array<std::unique_ptr<Slot[]>, MAX_INSTRUMENTS / CHUNK_SIZE> chunks_;
    std::atomic<uint32_t> count_{0};
    // Writer only: what it lists and holds, by broker, and what the config takes.
    std::array<StringSet, BROKER_COUNT> listed_;
    std::array<StringSet, BROKER_COUNT> held_;
    StringSet symbols_;
    std::vector<std::string> prefixes_;
    bool all_ = true;     // no "instruments" list: everything
    bool full_ = false;   // MAX_INSTRUMENTS listed, and said so
};

} // namespace slick::trader
