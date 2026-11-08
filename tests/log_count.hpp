#pragma once

// Counts log lines by a text their format string holds, so a test can tell a line went out once,
// or at all. test_main.cpp adds the sink before the logger starts: one added later would race the
// logger's writer thread.

#include <slick/logger.hpp>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace slick::trader::test {

class LogCountSink : public slick::logger::ISink {
public:
    static const std::shared_ptr<LogCountSink>& instance() {
        static const auto sink = std::make_shared<LogCountSink>();
        return sink;
    }

    void write(const slick::logger::LogEntry& entry) override {
        std::lock_guard lock(mutex_);
        const char* format = entry.format_ptr();
        if (!text_.empty() && format != nullptr && std::strstr(format, text_.c_str()) != nullptr) {
            ++count_;
        }
    }
    void flush() override {}

    void watch(std::string text) {
        std::lock_guard lock(mutex_);
        text_ = std::move(text);
        count_ = 0;
    }
    size_t count() {
        std::lock_guard lock(mutex_);
        return count_;
    }

private:
    std::mutex mutex_;
    std::string text_;
    size_t count_ = 0;
};

// The lines logged at WARN or above, from when it is made, whose format string holds `text`.
class LogCount {
public:
    explicit LogCount(std::string text) { LogCountSink::instance()->watch(std::move(text)); }
    ~LogCount() { LogCountSink::instance()->watch({}); }
    LogCount(const LogCount&) = delete;
    LogCount& operator=(const LogCount&) = delete;

    size_t count() const {
        slick::logger::Logger::instance().flush();
        return LogCountSink::instance()->count();
    }
};

} // namespace slick::trader::test
