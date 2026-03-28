# IdentifyService

## 功能描述

`IdentifyService` 实现 libp2p Identify 协议（`/ipfs/id/1.0.0`），允许节点互相交换身份信息：PeerId、监听地址列表、支持的协议列表、客户端版本等。

连接建立后，节点主动向对端发送自身的 `PeerInfo`，同时接收并存储对端信息。

## 接口定义

### PeerInfo

```cpp
struct PeerInfo {
    PeerId                 peer_id;
    std::vector<Multiaddr> listen_addrs;
    std::string            agent_version;
    std::string            protocol_version;
    std::vector<ProtocolId> protocols;
};
```

节点的完整公开身份信息，不包含私钥。

### IdentifyService

```cpp
class IdentifyService : public ProtocolHandler {
public:
    static constexpr std::string_view kProtocolId = "/ipfs/id/1.0.0";

    ProtocolId protocol_id() const override;

    // 入站：接收对端发来的 PeerInfo
    void on_inbound_stream(StreamHandle stream) override;

    // 出站：向对端发送本节点的 PeerInfo
    void on_outbound_stream_ready(StreamHandle stream) override;

    void on_tick() override;

    using IdentifyCallback = std::function<void(std::optional<PeerInfo>)>;

    // 在已就绪的出站流上发起 identify 交换
    // 成功时 callback 接收对端 PeerInfo；失败时接收 nullopt
    void identify(StreamHandle stream, IdentifyCallback callback);
};
```

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 实现 `ProtocolHandler` | 通过 `Swarm::register_handler()` 注册 |
| 由 `Node` 持有 | `Node::identify()` 返回其引用 |
| 使用 `MuxedStream` | 通过流发送/接收序列化的 PeerInfo |
| 写入 `PeerStore` | 收到对端地址后调用 `PeerStore::add_addr()` |
| 产出 `SwarmEvent::PeerIdentified` | 识别完成后通知 Swarm，由 Controller 可能触发进一步操作 |
| `PeerInfo.protocols` | 可用于动态发现对端支持的协议，再决定是否打开其他协议流 |
