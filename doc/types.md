# 核心类型（types.hpp）

## 功能描述

`types.hpp` 定义了整个 PeerCore 库共用的基础数据类型，是所有模块的类型基础。所有公开头文件均直接或间接依赖此文件。

## 接口定义

### PeerId

```cpp
struct PeerId {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const PeerId&) const = default;
    std::string to_string() const;
    static PeerId from_bytes(std::span<const uint8_t, 32> b);
};
```

32 字节 Ed25519 公钥，用作节点的唯一身份标识。支持相等比较、十六进制字符串转换，以及从字节序列构造。

### Identity

```cpp
struct Identity {
    PeerId peer_id;
    std::array<uint8_t, 64> secret_key{};  // Ed25519 密钥对（seed || pubkey）
};
```

节点完整身份，包含公钥（peer_id）和 64 字节私钥。仅在 `Node::Options` 内部持有，不对外暴露原始密钥。

### Multiaddr

```cpp
struct Multiaddr {
    struct Ip4TcpEndpoint {
        std::string ip;
        uint16_t    port{0};
    };

    std::vector<uint8_t> bytes;

    explicit Multiaddr(std::string_view text);
    std::string to_string() const;

    Result<Ip4TcpEndpoint> parse_ip4_tcp() const;
    static Multiaddr from_ip4_tcp(std::string_view ip, uint16_t port);
};
```

多协议地址（Multiaddr 规范子集）。内部以序列化字节存储，按需解码。目前支持 `/ip4/<addr>/tcp/<port>` 格式。

- `parse_ip4_tcp()`：将地址解析为 IP + 端口结构，返回 `Result` 以报告格式错误。
- `from_ip4_tcp()`：从 IP 字符串和端口构造 Multiaddr，用于 `sockaddr_in` 反向转换。
- 构造函数为 `explicit`，不存在默认构造，每个实例都保证持有合法格式的地址字节。

### 句柄与 ID 类型

```cpp
using ConnectionId = uint64_t;
using StreamId     = uint32_t;
using ProtocolId   = std::string;  // 例如 "/ipfs/ping/1.0.0"
```

用于在事件和接口间传递连接、流、协议的引用，避免传递指针。

### 字节视图

```cpp
using ConstBytes   = std::span<const uint8_t>;
using MutableBytes = std::span<uint8_t>;
```

零拷贝字节区间，用于读写接口。

### Result\<T\>

```cpp
template <typename T>
class Result {
public:
    static Result ok(T value);
    static Result err(std::string msg);

    bool is_ok()  const;
    bool is_err() const;

    T&       value();
    const T& value() const;
    const Error& error() const;
};

template <>
class Result<void> {
public:
    static Result ok();
    static Result err(std::string msg);

    bool is_ok()  const;
    bool is_err() const;
    const std::string& error_message() const;
};
```

类似 Rust `Result<T, E>` 的错误传播类型，内部使用 `std::variant<T, Error>`。`Result<void>` 为特化版本，用于只需成功/失败信息的操作。库中所有可能失败的操作均返回 `Result`，不使用异常。

### DebugSnapshot

```cpp
struct DebugSnapshot {
    uint32_t    connection_count{0};
    uint32_t    stream_count{0};
    std::string extra;
};
```

运行时状态快照，由 `Node::snapshot()` 和 `Swarm::snapshot()` 返回，用于调试和监控。

## 与其他模块的关系

| 依赖方向 | 说明 |
|----------|------|
| 被所有模块依赖 | `types.hpp` 是最底层头文件，不依赖任何其他 peercore 头文件 |
| `PeerId` | 被 `PeerStore`、`RoutingTable`、`ConnectionSession`、所有 Service 使用 |
| `Multiaddr` | 被 `Node`、`Swarm`、`PeerStore`、`TcpTransport`、`ConnectionSession`、`IdentifyService` 使用 |
| `Result<T>` | 所有可能失败的公开方法均以此为返回类型 |
| `ConnectionId` / `StreamId` | 在 `events.hpp`、`ConnectionSession`、`MuxedStream`、`Swarm` 中流通 |
