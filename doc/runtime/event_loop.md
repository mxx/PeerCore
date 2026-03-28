# EventLoop（runtime）

## 功能描述

`EventLoop` 是 PeerCore 的 I/O 事件循环，封装了平台特定的 I/O 多路复用机制：

- **macOS / BSD**：使用 `kqueue`（`EVFILT_READ` + `EVFILT_WRITE`，`EV_CLEAR` 边沿触发）
- **Linux**：使用 `epoll`（`EPOLLIN | EPOLLOUT | EPOLLET` 边沿触发）

提供统一接口：注册文件描述符的 I/O 回调，以及毫秒精度的定时器。

## 接口定义

```cpp
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // FD 管理
    void register_fd(Fd fd, std::function<void(IoEvent)> callback);
    void unregister_fd(Fd fd);

    // 定时器
    TimerId add_timer(std::chrono::milliseconds delay,
                      std::function<void()> callback,
                      bool repeat = false);
    void cancel_timer(TimerId id);

    // 驱动
    void poll_once();  // 非阻塞，处理当前就绪事件后返回
    void run();        // 阻塞运行直到 stop()
    void stop();
};

enum class IoEvent { Readable, Writable, Error };
```

## 事件分发

```
poll_with_timeout(ms)
    ├─ kevent() / epoll_wait() → 最多 64 个就绪事件
    ├─ 按 fd 查找 handler → 调用 callback(IoEvent)
    └─ fire_timers() → 触发所有到期定时器
```

- `run()` 循环调用 `poll_with_timeout(next_timer_delay())`，在两次 I/O 轮询之间的等待时间不超过最近定时器的剩余时间。
- 非重复定时器触发后自动移除；重复定时器的 deadline 向前推进一个 interval。

## 日志集成

EventLoop 集成了 `peercore/log.hpp`，组件名 `"peercore/runtime"`：

| 事件 | 日志级别 |
|------|----------|
| 构造成功 / 析构 | DEBUG |
| register_fd / unregister_fd | TRACE |
| add_timer / cancel_timer | TRACE |
| fire_timer | TRACE |
| event loop started / stopped | INFO |
| kqueue/epoll 创建失败 | ERROR（随后抛出异常） |
| kevent/epoll_ctl 注册失败 | ERROR（随后抛出异常） |

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 被 `Swarm` 使用 | Swarm 将连接 fd 注册到 EventLoop，在回调中驱动状态机 |
| 被 `TcpTransport` 使用 | listen fd 和 pending connect fd 注册到 EventLoop |
| 不属于公开 API | 位于 `src/runtime/`，未暴露在 `include/peercore/` 下 |
| 错误处理 | 构造失败和 fd 注册失败抛出 `std::runtime_error`（不可恢复） |
