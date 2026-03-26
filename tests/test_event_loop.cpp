#include <gtest/gtest.h>
#include "../src/runtime/event_loop.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <thread>

using namespace peercore::runtime;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Pipe {
    int read_fd{-1};
    int write_fd{-1};

    Pipe() {
        int fds[2];
        if (::pipe(fds) != 0) throw std::runtime_error("pipe() failed");
        read_fd  = fds[0];
        write_fd = fds[1];
        set_nonblocking(read_fd);
        set_nonblocking(write_fd);
    }
    ~Pipe() {
        if (read_fd  >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);
    }
};

// ── Timer tests ───────────────────────────────────────────────────────────────

TEST(EventLoop, TimerFiresAfterDelay) {
    EventLoop loop;
    int fired = 0;

    loop.add_timer(10ms, [&]{ ++fired; });

    // poll_once with timeout covers the delay
    std::this_thread::sleep_for(15ms);
    loop.poll_once();

    EXPECT_EQ(fired, 1);
}

TEST(EventLoop, TimerDoesNotFireBeforeDelay) {
    EventLoop loop;
    int fired = 0;

    loop.add_timer(200ms, [&]{ ++fired; });
    loop.poll_once();   // called immediately, timer not due yet

    EXPECT_EQ(fired, 0);
}

TEST(EventLoop, RepeatingTimerFiresMultipleTimes) {
    EventLoop loop;
    int fired = 0;

    loop.add_timer(10ms, [&]{ ++fired; }, /*repeat=*/true);

    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(15ms);
        loop.poll_once();
    }

    EXPECT_GE(fired, 3);
}

TEST(EventLoop, CancelTimerPreventsCallback) {
    EventLoop loop;
    int fired = 0;

    auto id = loop.add_timer(10ms, [&]{ ++fired; });
    loop.cancel_timer(id);

    std::this_thread::sleep_for(15ms);
    loop.poll_once();

    EXPECT_EQ(fired, 0);
}

TEST(EventLoop, MultipleTimersFire) {
    EventLoop loop;
    int a = 0, b = 0;

    loop.add_timer(10ms, [&]{ ++a; });
    loop.add_timer(10ms, [&]{ ++b; });

    std::this_thread::sleep_for(15ms);
    loop.poll_once();

    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
}

// ── FD event tests ────────────────────────────────────────────────────────────

TEST(EventLoop, ReadableFiresWhenDataAvailable) {
    EventLoop loop;
    Pipe p;
    int readable_count = 0;

    loop.register_fd(p.read_fd, [&](IoEvent ev) {
        if (ev == IoEvent::Readable) ++readable_count;
    });

    // Write to the pipe — read end becomes readable
    char c = 'x';
    ASSERT_EQ(::write(p.write_fd, &c, 1), 1);

    loop.poll_once();

    EXPECT_GE(readable_count, 1);
    loop.unregister_fd(p.read_fd);
}

TEST(EventLoop, WritableFiresOnWritableFd) {
    EventLoop loop;
    Pipe p;
    int writable_count = 0;

    // Write end of a pipe is immediately writable
    loop.register_fd(p.write_fd, [&](IoEvent ev) {
        if (ev == IoEvent::Writable) ++writable_count;
    });

    loop.poll_once();

    EXPECT_GE(writable_count, 1);
    loop.unregister_fd(p.write_fd);
}

TEST(EventLoop, UnregisterStopsEvents) {
    EventLoop loop;
    Pipe p;
    int count = 0;

    loop.register_fd(p.write_fd, [&](IoEvent) { ++count; });
    loop.unregister_fd(p.write_fd);

    loop.poll_once();

    EXPECT_EQ(count, 0);
}

TEST(EventLoop, ReadDataAfterReadableEvent) {
    EventLoop loop;
    Pipe p;
    std::string received;

    loop.register_fd(p.read_fd, [&](IoEvent ev) {
        if (ev != IoEvent::Readable) return;
        char buf[64]{};
        ssize_t n = ::read(p.read_fd, buf, sizeof(buf));
        if (n > 0) received.append(buf, n);
    });

    const char* msg = "hello";
    ::write(p.write_fd, msg, 5);

    loop.poll_once();

    EXPECT_EQ(received, "hello");
    loop.unregister_fd(p.read_fd);
}
