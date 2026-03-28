# ConnectionSession

## 功能描述

`ConnectionSession` 是单条 TCP 连接的完整生命周期管理器，封装了从原始 socket 到安全多路复用信道的整个升级过程：

```
TcpConnecting → TcpAccepted → Securing → SecureReady → Multiplexing → Ready → Closing → Closed
```

每个连接对应一个独立的 `ConnectionSession` 实例，由 `Swarm` 持有和驱动。

## 接口定义

### 状态机

```cpp
enum class ConnectionState {
    TcpConnecting,   // 发起出站连接，等待 connect() 完成
    TcpAccepted,     // 已接受入站 TCP 连接
    Securing,        // Noise 握手进行中
    SecureReady,     // 加密信道就绪
    Multiplexing,    // Yamux 协商中
    Ready,           // 可用于打开/接受流
    Closing,         // 关闭中
    Closed,          // 已完全关闭
    Failed,          // 不可恢复错误
};
```

### 抽象接口

```cpp
class ConnectionSession {
public:
    virtual ConnectionId    id()          const = 0;
    virtual ConnectionState state()       const = 0;
    virtual std::optional<PeerId> remote_peer() const = 0;

    // 由事件循环在 socket 就绪时调用
    virtual void on_socket_readable() = 0;
    virtual void on_socket_writable() = 0;
    virtual void on_timeout()         = 0;

    // 升级握手
    virtual Result<void> begin_outbound_upgrade() = 0;
    virtual Result<void> begin_inbound_upgrade()  = 0;

    // 流操作
    virtual Result<StreamHandle>        request_open_stream(const ProtocolId&) = 0;
    virtual std::optional<StreamHandle> accept_inbound_stream()                = 0;

    // 事件队列（由 Swarm 轮询）
    virtual std::optional<ConnectionEvent> next_event() = 0;

    virtual void close() = 0;
};
```

### 工厂函数

```cpp
std::unique_ptr<ConnectionSession> make_outbound_connection_session(
    ConnectionId id, int socket_fd, Multiaddr remote_addr);

std::unique_ptr<ConnectionSession> make_inbound_connection_session(
    ConnectionId id, int socket_fd, Multiaddr remote_addr);
```

出站和入站连接在握手方向上有区别（Noise 的 Initiator/Responder），工厂函数封装这一差异。

## ConnectionEvent

```cpp
struct ConnectionEvent {
    enum class Type {
        Secured,            // Noise 握手完成
        MultiplexerReady,   // Yamux 就绪
        StreamOpened,       // 本端打开了新流
        StreamAccepted,     // 对端打开了新流
        StreamClosed,       // 流已关闭
        Error,              // 协议错误
        Closed,             // 连接已关闭
    };
    Type                    type;
    std::optional<StreamId> stream_id;
    std::string             detail;
};
```

`Swarm` 通过轮询 `next_event()` 消费这些事件，并将其翻译为 `SwarmEvent`。

## 协议栈升级过程

```
socket_fd
    └─ Noise (加密层)      begin_*_upgrade() 启动
         └─ Yamux (多路复用层)  Noise 完成后自动启动
              └─ multistream-select (协议协商)  每条流上运行
```

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 由 `Swarm` 持有 | Swarm 创建、驱动并销毁 ConnectionSession |
| 使用 `MuxedStream` | `request_open_stream` / `accept_inbound_stream` 返回 `StreamHandle` |
| 依赖 `TcpTransport` | socket_fd 由 TcpTransport 在 accept/connect 成功后提供 |
| 依赖 Noise 实现 | `begin_*_upgrade()` 内部调用 Noise 握手逻辑 |
| 依赖 Yamux 实现 | Noise 完成后初始化 Yamux 多路复用器 |
| 产出 `ConnectionEvent` | 由 Swarm 消费，翻译为 SwarmEvent |
