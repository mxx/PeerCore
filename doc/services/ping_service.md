# PingService

## 功能描述

`PingService` 实现 libp2p Ping 协议（`/ipfs/ping/1.0.0`），用于测量两个节点之间的往返时延（RTT）并验证连通性。

协议行为：向对端发送随机 32 字节 payload，对端原样回响，通过时间差计算 RTT。

## 接口定义

```cpp
class PingService : public ProtocolHandler {
public:
    static constexpr std::string_view kProtocolId = "/ipfs/ping/1.0.0";

    ProtocolId protocol_id() const override;

    // 入站：对端发起 ping，本端作为响应方，原样回写收到的 payload
    void on_inbound_stream(StreamHandle stream) override;

    // 出站：本端打开 ping 流后，发起测量
    void on_outbound_stream_ready(StreamHandle stream) override;

    // 定时回调，处理超时等
    void on_tick() override;

    using RttCallback = std::function<void(std::optional<std::chrono::milliseconds>)>;

    // 在已就绪的出站流上发起单次 ping 测量
    // 成功时 callback 接收 RTT；超时或错误时接收 nullopt
    void ping(StreamHandle stream, RttCallback callback);
};
```

## 使用方式

```cpp
// 通过 Node 访问
node.ping().ping(stream, [](std::optional<std::chrono::milliseconds> rtt) {
    if (rtt) {
        // 连接正常，RTT = *rtt
    } else {
        // ping 失败或超时
    }
});
```

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 实现 `ProtocolHandler` | 通过 `Swarm::register_handler()` 注册 |
| 由 `Node` 持有 | `Node::ping()` 返回其引用 |
| 使用 `MuxedStream` | 通过 `StreamHandle` 读写 ping payload |
| 协议标识 | `/ipfs/ping/1.0.0`，由 multistream-select 协商 |
