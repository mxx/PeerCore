#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace peercore::runtime {

using Fd      = int;
using TimerId = uint64_t;

enum class IoEvent { Readable, Writable, Error };

struct FdHandler {
    Fd fd{-1};
    std::function<void(IoEvent)> callback;
};

struct Timer {
    TimerId id{0};
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> callback;
    bool repeat{false};
    std::chrono::milliseconds interval{0};
};

// I/O event loop backed by kqueue (macOS/BSD) or epoll (Linux).
// Drives ConnectionSession I/O callbacks and periodic timers.
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void register_fd(Fd fd, std::function<void(IoEvent)> callback);
    void unregister_fd(Fd fd);

    TimerId add_timer(std::chrono::milliseconds delay,
                      std::function<void()> callback,
                      bool repeat = false);
    void cancel_timer(TimerId id);

    // Non-blocking: process ready FDs and due timers, then return immediately
    void poll_once();

    // Blocking run until stop() is called; sleeps between iterations
    void run();
    void stop();

private:
    void poll_with_timeout(long timeout_ms);
    int  queue_fd_{-1};   // kqueue fd (macOS) or epoll fd (Linux)
    bool running_{false};

    std::vector<FdHandler> fd_handlers_;
    std::vector<Timer>     timers_;
    TimerId                next_timer_id_{1};

    void fire_timers();
    std::chrono::milliseconds next_timer_delay() const;
};

}  // namespace peercore::runtime
