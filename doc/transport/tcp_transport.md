# TcpTransport

## 功能描述

`TcpTransport` 负责 TCP 层的非阻塞套接字管理，提供两个核心能力：

1. **监听**（`listen()`）：绑定并监听本地地址，接受入站连接。
2. **拨号**（`dial()`）：发起非阻塞出站 TCP 连接。

`TcpTransport` 本身不参与协议升级，仅负责 TCP 连接的建立阶段，完成后通过回调将 `TcpSocket`（裸 fd + 远端地址）交给上层。

`TcpTransport` 是内部模块，位于 `src/transport/`，未暴露于 `include/peercore/`。

## 接口定义

### TcpSocket

```cpp
struct TcpSocket {
    RawFd fd{-1};
    Multiaddr remote_addr;
};
```

回调传出的 TCP 套接字，调用方取得所有权，**必须在使用完后自行 close(fd)**。

### TcpTransportCallbacks

```cpp
struct TcpTransportCallbacks {
    std::function<void(TcpSocket)>              on_accepted;    // 入站连接就绪
    std::function<void(TcpSocket)>              on_connected;   // 出站连接成功
    std::function<void(const Multiaddr&, std::string)> on_dial_failed;  // 出站连接失败
};
```

### TcpTransport

```cpp
class TcpTransport {
public:
    Result<void> listen(const Multiaddr& addr, TcpTransportCallbacks callbacks);

    // 返回 ok 表示"拨号已发起"，而非"连接已成功"。
    // 连接结果通过 on_connected / on_dial_failed 异步通知。
    // 仅地址格式错误或 socket() 失败时返回 err。
    Result<void> dial(const Multiaddr& addr, TcpTransportCallbacks callbacks);

    // 由 EventLoop 在监听 fd 可读时调用，内部循环 accept() 直到 EAGAIN
    void on_accept_ready(RawFd listen_fd);
    // 由 EventLoop 在 connect fd 可写时调用，检查 SO_ERROR
    void on_connect_ready(RawFd socket_fd);

    std::vector<RawFd> listener_fds() const;
    std::vector<RawFd> dialing_fds()  const;
    Result<Multiaddr>  local_addr(RawFd fd) const;

    // 关闭并清空所有监听器和待连接项，可安全多次调用，析构时自动调用
    void close_all();
};
```

## 连接建立流程

### 入站

```
listen(addr) → create_listen_socket() → bind() → listen()
EventLoop 通知 fd 可读
    └─ on_accept_ready(listen_fd)
         └─ accept() 循环直到 EAGAIN
              └─ set_nonblocking(accepted_fd)
                   └─ on_accepted(TcpSocket{fd, remote_addr})
```

### 出站

```
dial(addr) → create_connect_socket() → connect()
    ├─ rc==0 (立即成功)     → on_connected(TcpSocket)
    ├─ errno==EINPROGRESS   → 记录 PendingDial，注册到 EventLoop
    │       EventLoop 通知 fd 可写
    │           └─ on_connect_ready(fd) → getsockopt(SO_ERROR)
    │                ├─ error==0         → on_connected
    │                └─ error!=0         → on_dial_failed (WARN 日志)
    └─ 其他 errno           → on_dial_failed (WARN 日志)
```

## 日志集成

组件名 `"peercore/tcp"`：

| 事件 | 级别 |
|------|------|
| listen 地址解析失败 / bind 失败 | WARN |
| listen 成功 | DEBUG |
| dial 立即连接成功 | DEBUG |
| dial 立即连接失败 | WARN |
| dial 进入 pending 状态 | TRACE |
| accept 成功（含 remote 地址）| DEBUG |
| accept 系统调用失败 | ERROR |
| accepted fd 设置 flags 失败 | ERROR |
| async dial 成功 | DEBUG |
| async dial 失败 | WARN |
| inet_ntop 失败 | ERROR |
| close_all 入口 | DEBUG |

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 被 `Swarm` 使用 | Swarm 创建 TcpTransport，将 listener_fds/dialing_fds 注册到 EventLoop |
| 向 `ConnectionSession` 移交 fd | on_accepted/on_connected 回调中创建 ConnectionSession |
| 依赖 `EventLoop` | on_accept_ready / on_connect_ready 由 EventLoop 的 I/O 回调触发 |
| 依赖 `Multiaddr` | 地址解析通过 `Multiaddr::parse_ip4_tcp()` 完成 |
