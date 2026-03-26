#include "event_loop.hpp"

#include <algorithm>
#include <stdexcept>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#  include <sys/event.h>
#  include <sys/time.h>
#  define PEERCORE_USE_KQUEUE 1
#else
#  include <sys/epoll.h>
#  define PEERCORE_USE_EPOLL 1
#endif

namespace peercore::runtime {

// ── Construction / destruction ────────────────────────────────────────────────

EventLoop::EventLoop() {
#if defined(PEERCORE_USE_KQUEUE)
    queue_fd_ = ::kqueue();
    if (queue_fd_ < 0) throw std::runtime_error("kqueue() failed");
#else
    queue_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (queue_fd_ < 0) throw std::runtime_error("epoll_create1() failed");
#endif
}

EventLoop::~EventLoop() {
    if (queue_fd_ >= 0) ::close(queue_fd_);
}

// ── FD registration ───────────────────────────────────────────────────────────

void EventLoop::register_fd(Fd fd, std::function<void(IoEvent)> callback) {
#if defined(PEERCORE_USE_KQUEUE)
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ,  EV_ADD | EV_CLEAR, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(queue_fd_, changes, 2, nullptr, 0, nullptr) < 0)
        throw std::runtime_error("kevent register failed");
#else
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    if (::epoll_ctl(queue_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::runtime_error("epoll_ctl EPOLL_CTL_ADD failed");
#endif
    fd_handlers_.push_back({fd, std::move(callback)});
}

void EventLoop::unregister_fd(Fd fd) {
#if defined(PEERCORE_USE_KQUEUE)
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(queue_fd_, changes, 2, nullptr, 0, nullptr);
#else
    ::epoll_ctl(queue_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
    fd_handlers_.erase(
        std::remove_if(fd_handlers_.begin(), fd_handlers_.end(),
                       [fd](const FdHandler& h) { return h.fd == fd; }),
        fd_handlers_.end());
}

// ── Timers ────────────────────────────────────────────────────────────────────

TimerId EventLoop::add_timer(std::chrono::milliseconds delay,
                              std::function<void()> callback,
                              bool repeat) {
    TimerId id = next_timer_id_++;
    timers_.push_back({id, std::chrono::steady_clock::now() + delay,
                       std::move(callback), repeat, delay});
    return id;
}

void EventLoop::cancel_timer(TimerId id) {
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
                       [id](const Timer& t) { return t.id == id; }),
        timers_.end());
}

// ── poll_once / run ───────────────────────────────────────────────────────────

// Dispatch pending I/O events with the given timeout (milliseconds).
// timeout=0 → non-blocking; timeout=-1 → block until next timer.
void EventLoop::poll_with_timeout(long timeout_ms) {
#if defined(PEERCORE_USE_KQUEUE)
    constexpr int kMaxEvents = 64;
    struct kevent events[kMaxEvents];
    struct timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1'000'000L};
    int n = ::kevent(queue_fd_, nullptr, 0, events, kMaxEvents,
                     timeout_ms < 0 ? nullptr : &ts);
    for (int i = 0; i < n; ++i) {
        Fd fd = static_cast<Fd>(events[i].ident);
        for (auto& h : fd_handlers_) {
            if (h.fd == fd) {
                if (events[i].filter == EVFILT_READ)  h.callback(IoEvent::Readable);
                if (events[i].filter == EVFILT_WRITE) h.callback(IoEvent::Writable);
                if (events[i].flags  & EV_ERROR)      h.callback(IoEvent::Error);
                break;
            }
        }
    }
#else
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];
    int n = ::epoll_wait(queue_fd_, events, kMaxEvents, static_cast<int>(timeout_ms));
    for (int i = 0; i < n; ++i) {
        Fd fd = events[i].data.fd;
        for (auto& h : fd_handlers_) {
            if (h.fd == fd) {
                if (events[i].events & EPOLLIN)  h.callback(IoEvent::Readable);
                if (events[i].events & EPOLLOUT) h.callback(IoEvent::Writable);
                if (events[i].events & EPOLLERR) h.callback(IoEvent::Error);
                break;
            }
        }
    }
#endif

    fire_timers();
}

void EventLoop::poll_once() {
    poll_with_timeout(0);
}

void EventLoop::run() {
    running_ = true;
    while (running_) {
        poll_with_timeout(next_timer_delay().count());
    }
}

void EventLoop::stop() { running_ = false; }

// ── Helpers ───────────────────────────────────────────────────────────────────

void EventLoop::fire_timers() {
    auto now = std::chrono::steady_clock::now();
    for (auto& t : timers_) {
        if (now >= t.deadline) {
            t.callback();
            if (t.repeat) t.deadline = now + t.interval;
        }
    }
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
                       [](const Timer& t) {
                           return !t.repeat &&
                                  std::chrono::steady_clock::now() >= t.deadline;
                       }),
        timers_.end());
}

std::chrono::milliseconds EventLoop::next_timer_delay() const {
    if (timers_.empty()) return std::chrono::milliseconds(10);
    auto now = std::chrono::steady_clock::now();
    auto it  = std::min_element(timers_.begin(), timers_.end(),
        [](const Timer& a, const Timer& b) { return a.deadline < b.deadline; });
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(it->deadline - now);
    return diff.count() > 0 ? diff : std::chrono::milliseconds(0);
}

}  // namespace peercore::runtime
