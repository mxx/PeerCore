#include "peercore/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace peercore::log {

namespace {

void default_sink(Level level, std::string_view component, std::string_view msg) {
    static constexpr const char* kNames[] = {
        "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR"
    };
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d][%s][%.*s] %.*s\n",
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<int>(ms.count()),
        kNames[static_cast<int>(level)],
        static_cast<int>(component.size()), component.data(),
        static_cast<int>(msg.size()), msg.data());
}

std::mutex g_sink_mutex;
Sink       g_sink;                                          // null → default_sink
std::atomic<int> g_level{static_cast<int>(Level::Info)};   // runtime gate

} // namespace

void set_sink(Sink sink) {
    std::lock_guard lk(g_sink_mutex);
    g_sink = std::move(sink);
}

void set_level(Level level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level get_level() {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void write(Level level, std::string_view component, std::string_view msg) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;

    std::lock_guard lk(g_sink_mutex);
    if (g_sink) g_sink(level, component, msg);
    else        default_sink(level, component, msg);
}

} // namespace peercore::log
