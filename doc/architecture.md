# PeerCore 架构设计

## 1. 设计目标

实现一个 C++20 编写的最小 libp2p 子集库，满足：

- 与 rust-libp2p 节点互通
- 尽量减少外部依赖（仅保留必要密码学原语）
- 提供清晰的架构边界，避免后期失控
- 支持调试、观测和后续扩展

---

## 2. 核心设计原则

### 2.1 分层明确

系统划分为四个核心层：

```
Node → Swarm → ConnectionSession → MuxedStream
```

每层职责单一，禁止跨层耦合。

### 2.2 显式状态机

- 所有连接、流、协议状态必须显式建模
- 不允许隐藏在回调或 coroutine frame 中

### 2.3 不强绑定 coroutine

- 核心 API 不依赖 `co_await`
- coroutine 作为可选适配层存在

### 2.4 统一事件模型

- 所有网络行为通过统一事件流表达
- Swarm 是唯一事件汇聚点

### 2.5 最小依赖原则

允许依赖：
- 密码学原语（哈希、签名、AEAD）

避免依赖：
- Asio / libuv 等大型网络框架

---

## 3. 总体架构

```
Node
  ├─ Identity
  ├─ Services (DHT / Ping / Identify)
  ├─ PeerStore
  └─ Swarm
        ├─ ConnectionSession (N)
        │     ├─ Noise
        │     └─ Yamux
        ├─ ProtocolHandlers
        └─ Event Loop
```

---

## 4. 模块定义

### 4.1 Node（对外接口层）

**职责**
- 节点生命周期管理
- 提供用户 API
- 聚合协议服务
- 封装 Swarm

**接口**

```cpp
class Node {
public:
  struct Options {
    Identity identity;
    std::vector<Multiaddr> listen_addrs;
    std::vector<Multiaddr> bootstrap_addrs;
  };

  explicit Node(Options opts);

  Result<void> start();
  Result<void> stop();

  Result<void> connect(const Multiaddr& addr);
  Result<StreamHandle> open_stream(const PeerId&, const ProtocolId&);

  DhtService& dht();
  PingService& ping();
  IdentifyService& identify();

  DebugSnapshot snapshot() const;
};
```

---

### 4.2 Swarm（网络运行时核心）

**职责**
- 管理连接生命周期
- 管理流
- 分发事件
- 驱动协议
- 调度 dial / listen

**接口**

```cpp
class Swarm {
public:
  Result<void> start();
  Result<void> stop();

  Result<void> listen_on(const Multiaddr&);
  Result<void> dial_addr(const Multiaddr&);
  Result<void> dial_peer(const PeerId&);

  Result<StreamHandle> open_stream(ConnectionId, ProtocolId);

  void poll_once();
  std::optional<SwarmEvent> next_event();

  DebugSnapshot snapshot() const;
};
```

---

### 4.3 SwarmEvent（统一事件）

```cpp
struct SwarmEvent {
  enum class Type {
    ListenerStarted,
    IncomingConnection,
    ConnectionEstablished,
    ConnectionClosed,
    DialFailed,
    StreamOpened,
    StreamAccepted,
    StreamClosed,
    ProtocolNegotiated,
    ProtocolError,
    PeerIdentified
  };

  Type type;
  ConnectionId connection_id;
  std::optional<StreamId> stream_id;
  std::optional<PeerId> peer_id;
  std::string detail;
};
```

---

### 4.4 ConnectionSession（连接状态机）

**状态**

```cpp
enum class ConnectionState {
  TcpConnecting,
  TcpAccepted,
  Securing,
  SecureReady,
  Multiplexing,
  Ready,
  Closing,
  Closed,
  Failed
};
```

**职责**
- TCP 生命周期
- Noise 握手
- Yamux 初始化
- stream 管理
- 超时处理

**接口**

```cpp
class ConnectionSession {
public:
  ConnectionId id() const;
  ConnectionState state() const;

  std::optional<PeerId> remote_peer() const;

  void on_socket_readable();
  void on_socket_writable();
  void on_timeout();

  Result<void> begin_outbound_upgrade();
  Result<void> begin_inbound_upgrade();

  Result<StreamHandle> request_open_stream(const ProtocolId&);
  std::optional<StreamHandle> accept_inbound_stream();

  std::optional<ConnectionEvent> next_event();

  void close();
};
```

---

### 4.5 MuxedStream（逻辑流）

**职责**
- 数据读写
- 流关闭/重置
- 协议绑定

**接口**

```cpp
class MuxedStream {
public:
  StreamId id() const;
  ConnectionId connection_id() const;

  Result<size_t> try_read(MutableBytes);
  Result<size_t> try_write(ConstBytes);

  Result<void> close_write();
  Result<void> reset();

  bool is_open() const;

  std::optional<ProtocolId> negotiated_protocol() const;
};
```

---

### 4.6 ProtocolHandler（协议插件）

**职责**
- 处理协议逻辑
- 响应 inbound stream
- 管理协议状态

**接口**

```cpp
class ProtocolHandler {
public:
  virtual ~ProtocolHandler() = default;

  virtual ProtocolId protocol_id() const = 0;

  virtual void on_inbound_stream(StreamHandle) = 0;
  virtual void on_outbound_stream_ready(StreamHandle) = 0;

  virtual void on_tick() = 0;
};
```

---

### 4.7 PeerStore（节点信息存储）

```cpp
class PeerStore {
public:
  void add_addr(const PeerId&, const Multiaddr&);
  std::vector<Multiaddr> get_addrs(const PeerId&) const;

  void record_dial_success(const PeerId&);
  void record_dial_failure(const PeerId&);
};
```

---

### 4.8 RoutingTable（Kademlia）

```cpp
class RoutingTable {
public:
  void insert(const PeerId&);
  void remove(const PeerId&);
  std::vector<PeerId> closest_peers(const PeerId&, size_t k) const;
};
```

---

### 4.9 Controller（策略层）

```cpp
class Controller {
public:
  void on_swarm_event(const SwarmEvent&);
  void on_timer_tick();

  std::vector<Action> drain_actions();
};
```

---

## 5. 模块边界总结

| 模块 | 职责 | 不负责 |
|------|------|--------|
| Node | 对外接口 | IO、frame |
| Swarm | 网络调度 | 协议细节 |
| Connection | 协议升级 | 全局策略 |
| Stream | 数据通道 | socket |
| Protocol | 业务逻辑 | 连接管理 |

---

## 6. 事件流模型

```
OS Event
→ ConnectionSession
→ Swarm
→ ProtocolHandler / Controller
→ Action
→ Swarm 执行
```

---

## 7. Coroutine 策略

**核心原则**
- 核心层：无 coroutine
- 上层：可选 coroutine

**示例**

```cpp
Task<StreamHandle> async_open_stream(Node&, PeerId, ProtocolId);
```

---

## 8. 调试与可观测性

必须内建：
- 事件日志（带 peer_id / conn_id / stream_id）
- 状态快照
- 最近失败记录
- 协议 dump（可选）

---

## 9. 第一阶段实现范围

支持协议栈：

```
TCP → Noise → Yamux → multistream-select → Identify / Ping / Kad（最小）
```

---

## 10. 实现顺序

1. runtime（epoll / event loop）
2. TCP transport
3. multistream-select
4. Noise
5. Yamux
6. Identify
7. Ping
8. Kad（最小）

---

## 11. 结论

该架构定义：
- 清晰的 Node / Swarm 边界
- 显式连接状态机
- 可扩展协议系统
- 与 libp2p 规范兼容

**核心思想：**

> 用显式状态机和统一事件流构建 libp2p，而不是依赖隐式异步控制流。
