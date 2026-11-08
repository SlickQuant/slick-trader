#pragma once

#include <slick-trader/strategy/strategy_base.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slick::trader {

// Lightweight strategy that owns all orders placed manually through the Web UI.
// Always registered at startup. It adopts the instruments its config lists as brokers define
// them, so the Web UI can ladder and trade them with no strategy plugin loaded. In
// MANUAL.config.json (any <name>.config.json naming it), parameter "instruments": symbols, or
// prefixes ending in '*' ("ZN*", or "*" for all). None when absent.
class ManualStrategy : public StrategyBase {
public:
    static constexpr const char* ID = "MANUAL";
    static constexpr uint32_t CHUNK_SIZE = 256;
    static constexpr uint32_t MAX_INSTRUMENTS = CHUNK_SIZE * 256;
    // What it subscribes of each instrument it adopts.
    static constexpr SubscriptionType SUBSCRIPTION = SubscriptionType::LEVEL2;

    ManualStrategy() : StrategyBase(ID, "1.0.0") {}

    // The level 2 book of everything adopted so far, one request per broker. Safe from any
    // thread: the WebServer reads it while the StrategyManager thread adopts.
    std::vector<SubscriptionRequest> get_subscriptions() const override {
        std::vector<SubscriptionRequest> subs;
        for_each_adopted(0, [&](BrokerId broker, std::string_view instrument) {
            auto it = std::find_if(subs.begin(), subs.end(),
                                   [&](const SubscriptionRequest& s) { return s.broker_id == broker; });
            if (it == subs.end()) {
                it = subs.insert(subs.end(), SubscriptionRequest(broker, SUBSCRIPTION, {}));
            }
            it->instruments.emplace_back(instrument);
        });
        return subs;
    }

    // Calls `f(broker, instrument)` for each instrument adopted from the `from`th on, in the
    // order adopted, and returns how many are adopted: passed back as `from`, it hands over only
    // what is new. Safe from any thread, as get_subscriptions() is.
    template <typename F>
    uint32_t for_each_adopted(uint32_t from, F&& f) const {
        const uint32_t count = count_.load(std::memory_order_acquire);
        for (uint32_t i = from; i < count; ++i) {
            const Slot& slot = chunks_[i / CHUNK_SIZE][i % CHUNK_SIZE];
            f(slot.broker, std::string_view(slot.instrument));
        }
        return count;
    }

    uint32_t adopted_count() const noexcept { return count_.load(std::memory_order_acquire); }

    // Takes on an instrument a broker defined. False when the config does not list it, when
    // already held, when full, or when no broker is named: one of UNKNOWN takes no order and has
    // no request queue. StrategyManager thread only: it is the one writer.
    bool adopt(BrokerId broker, std::string_view instrument) {
        const auto b = static_cast<size_t>(broker);
        // Held already is the common case, as a venue replays its definitions: found with no
        // string built.
        if (broker == BrokerId::UNKNOWN || b >= adopted_.size() || !lists(instrument) ||
            adopted_[b].contains(instrument)) {
            return false;
        }
        const uint32_t n = count_.load(std::memory_order_relaxed);
        if (n == MAX_INSTRUMENTS) [[unlikely]] {
            // Once: it stays full, and a venue's worth past it would each log a line.
            if (!full_) {
                full_ = true;
                LOG_WARN("Strategy {}: holding the maximum of {} instruments, so {} and any it is offered after "
                         "are not adopted",
                         ID, MAX_INSTRUMENTS, instrument);
            }
            return false;
        }
        adopted_[b].emplace(instrument);
        auto& chunk = chunks_[n / CHUNK_SIZE];
        if (!chunk) {
            chunk = std::make_unique<Slot[]>(CHUNK_SIZE);
        }
        Slot& slot = chunk[n % CHUNK_SIZE];
        slot.broker = broker;
        const size_t len = std::min(instrument.size(), sizeof(slot.instrument) - 1);
        std::memcpy(slot.instrument, instrument.data(), len);
        slot.instrument[len] = '\0';
        count_.store(n + 1, std::memory_order_release);
        return true;
    }

    // Whether the config lists `instrument`, by its symbol or a prefix.
    bool lists(std::string_view instrument) const {
        return symbols_.contains(instrument) ||
               std::ranges::any_of(prefixes_, [&](const std::string& prefix) { return instrument.starts_with(prefix); });
    }

protected:
    bool on_initialize() override { return true; }

    // At load, and on a parameter update from the Web UI, on the StrategyManager thread, which
    // then has the instruments brokers defined already adopted by the new list. An update replaces
    // the list, as it does every parameter, here and in the file it is saved to: one without
    // "instruments" empties it. What is held stays held.
    void on_config(const nlohmann::json& params) override {
        const bool listed = !symbols_.empty() || !prefixes_.empty();
        symbols_.clear();
        prefixes_.clear();
        const auto it = params.find("instruments");
        if (it == params.end() || !it->is_array()) {
            if (listed) {
                LOG_WARN("Strategy {}: the update has no \"instruments\" list, so it adopts no instrument from now on", ID);
            }
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
        LOG_INFO("Strategy {}: adopts {} symbol(s) and {} prefix(es) as brokers define them", ID, symbols_.size(),
                 prefixes_.size());
    }

private:
    struct Slot {
        BrokerId broker;
        char instrument[MAX_INSTRUMENT_LENGTH];
    };

    // Append-only, so readers take no lock: a slot is written before the count that covers
    // it is released, and never changes after. Chunks grow memory only with what is adopted.
    std::array<std::unique_ptr<Slot[]>, MAX_INSTRUMENTS / CHUNK_SIZE> chunks_;
    std::atomic<uint32_t> count_{0};
    // Writer only: what it holds, by broker, and what the config lists, by symbol and by prefix.
    std::array<StringSet, static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_)> adopted_;
    StringSet symbols_;
    std::vector<std::string> prefixes_;
    bool full_ = false;   // MAX_INSTRUMENTS held, and said so
};

} // namespace slick::trader
