# Node

## 功能描述

`Node` 是 PeerCore 的顶层入口类，负责组装并生命周期管理所有子系统：`PeerStore`、`Swarm`、三个内置服务（DHT / Ping / Identify）。对外提供最简洁的高层 API，屏蔽内部协议栈细节。

一个进程通常只持有一个 `Node` 实例。Node 不可拷贝。

## 接口定义

### Node::Options

```cpp
struct Options {
    Identity               identity;
    std::vector<Multiaddr> listen_addrs;
    std::vector<Multiaddr> bootstrap_addrs;
};
```

构造参数：
- `identity`：节点的 Ed25519 密钥对，决定 `PeerId`。
- `listen_addrs`：启动后监听的本地地址列表（例如 `/ip4/0.0.0.0/tcp/4001`）。
- `bootstrap_addrs`：启动时主动拨号的引导节点地址。

### 构造与生命周期

```cpp
explicit Node(Options opts);
~Node();

Result<void> start();
Result<void> stop();
```

- `start()`：启动事件循环、绑定监听地址、向引导节点发起连接。
- `stop()`：关闭所有连接，停止事件循环，释放资源。

### 网络操作

```cpp
Result<void>         connect(const Multiaddr& addr);
Result<StreamHandle> open_stream(const PeerId& peer, const ProtocolId& proto);
```

- `connect()`：向指定地址主动建立连接。
- `open_stream()`：向已连接的 peer 打开指定协议的流，返回 `StreamHandle`。

### 服务访问

```cpp
DhtService&      dht();
PingService&     ping();
IdentifyService& identify();
```

返回内置服务的引用，允许应用层直接触发 DHT 查询、Ping 测量、节点识别。

### 调试

```cpp
DebugSnapshot snapshot() const;
```

返回当前连接数、流数等状态摘要。

## 与其他模块的关系

```
Node
├── 持有 PeerStore        （地址 + 拨号历史）
├── 持有 Swarm            （连接管理 + 事件循环）
├── 持有 DhtService       （注册到 Swarm 的协议处理器）
├── 持有 PingService      （同上）
└── 持有 IdentifyService  （同上）
```

- `Node` 是唯一持有 `Identity`（私钥）的地方，其他模块只见 `PeerId`。
- `Swarm` 由 `Node` 构造并持有，`Node::start()` 委托给 `Swarm::start()`。
- 三个 Service 通过 `Swarm::register_handler()` 注册，`Node` 通过 `shared_ptr` 同时持有引用以暴露给用户。
- `connect()` / `open_stream()` 是对 `Swarm::dial_addr()` / `Swarm::open_stream()` 的薄封装。
