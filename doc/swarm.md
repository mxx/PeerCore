# Swarm

## 功能描述

`Swarm` 是连接管理的核心，负责：

1. 维护所有活跃 `ConnectionSession` 的生命周期
2. 驱动各连接的 I/O 状态机（`poll_once()`）
3. 将底层 `ConnectionEvent` 翻译为 `SwarmEvent` 并分发给协议处理器和 Controller
4. 执行 Controller 返回的 `Action`（拨号、关闭连接、打开流等）
5. 向上层提供事件轮询队列（`next_event()`）

## 接口定义

### 构造

```cpp
explicit Swarm(PeerStore& peer_store);
```

接受 `PeerStore` 引用（不持有所有权）。`PeerStore` 生命周期必须不短于 `Swarm`。

### 生命周期

```cpp
Result<void> start();
Result<void> stop();
```

### 传输操作

```cpp
Result<void> listen_on(const Multiaddr& addr);
Result<void> dial_addr(const Multiaddr& addr);
Result<void> dial_peer(const PeerId& peer);
```

- `dial_peer()`：从 `PeerStore` 查询已知地址，再调用 `dial_addr()`。若无已知地址则返回错误。

### 流操作

```cpp
Result<StreamHandle> open_stream(ConnectionId conn_id, ProtocolId proto);
```

### 协议注册与策略

```cpp
void register_handler(std::shared_ptr<ProtocolHandler> handler);
void set_controller(std::shared_ptr<Controller> ctrl);
```

- 可注册多个协议处理器；默认使用 `DefaultController`（空实现）。

### 事件驱动

```cpp
void poll_once();
std::optional<SwarmEvent> next_event();
```

- `poll_once()`：驱动所有连接的状态机，调用 Controller 的定时器，执行待处理 Action。
- `next_event()`：从内部事件队列取出一条 `SwarmEvent`，无则返回 `nullopt`。

### 调试

```cpp
DebugSnapshot snapshot() const;
```

## SwarmEvent

```cpp
struct SwarmEvent {
    enum class Type {
        ListenerStarted, IncomingConnection, ConnectionEstablished,
        ConnectionClosed, DialFailed, StreamOpened, StreamAccepted,
        StreamClosed, ProtocolNegotiated, ProtocolError, PeerIdentified,
    };
    Type                    type;
    ConnectionId            connection_id{0};
    std::optional<StreamId> stream_id;
    std::optional<PeerId>   peer_id;
    std::string             detail;
};
```

所有网络事件的统一表示，字段按需填充。

## 与其他模块的关系

```
Swarm
├── 依赖 PeerStore          （dial_peer 查地址）
├── 持有 ConnectionSession* （每条连接一个实例）
├── 持有 ProtocolHandler[]  （协议插件列表）
├── 持有 Controller         （策略层，默认为 DefaultController）
└── 产出 SwarmEvent         （向上层暴露，也传给 Controller 和 ProtocolHandler）
```

- `Swarm` 不直接感知传输层（TCP），传输层由 `ConnectionSession` 封装。
- `Controller` 是 Swarm 的决策注入点：Swarm 将事件推给 Controller，Controller 返回 Action，Swarm 执行。
- 每次 `poll_once()` 形成一个完整的响应周期：驱动 I/O → 翻译事件 → 查询策略 → 执行动作。
