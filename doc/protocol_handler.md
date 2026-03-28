# ProtocolHandler

## 功能描述

`ProtocolHandler` 是协议插件的抽象基类，定义了三个回调点，使自定义协议可以无侵入地接入 PeerCore 的流管理体系。所有内置服务（Ping、Identify、DHT）均实现此接口。

## 接口定义

```cpp
class ProtocolHandler {
public:
    virtual ~ProtocolHandler() = default;

    // 返回此处理器负责的协议标识符
    // 例如 "/ipfs/ping/1.0.0"
    virtual ProtocolId protocol_id() const = 0;

    // 对端发起了一条入站流并完成协议协商后调用
    virtual void on_inbound_stream(StreamHandle stream) = 0;

    // 本端主动打开一条流并完成协议协商后调用
    virtual void on_outbound_stream_ready(StreamHandle stream) = 0;

    // 每次 Swarm::poll_once() 时调用，用于定时任务
    virtual void on_tick() = 0;
};
```

## 注册与分发机制

```
Swarm::register_handler(handler)
    └─ Swarm 维护 handlers_ 列表

每次 Swarm::poll_once():
    └─ 每个 handler 的 on_tick() 被调用

当有新流产生（ConnectionEvent::StreamAccepted）:
    └─ Swarm 查找匹配 protocol_id 的 handler
         └─ 调用 on_inbound_stream(stream)

当 open_stream() 完成协商:
    └─ 调用对应 handler 的 on_outbound_stream_ready(stream)
```

## 实现指南

实现一个新协议处理器：

1. 继承 `ProtocolHandler`
2. `protocol_id()` 返回协议字符串（遵循 multistream-select 约定）
3. 在 `on_inbound_stream()` 中处理对端发起的请求
4. 在 `on_outbound_stream_ready()` 中发送请求
5. 在 `on_tick()` 中处理超时、重试等定时逻辑
6. 调用 `Swarm::register_handler(shared_ptr<ProtocolHandler>)` 注册

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 由 `Swarm` 管理 | `register_handler()` 注册，`poll_once()` 驱动 |
| 使用 `MuxedStream` | 所有数据交换通过 `StreamHandle` 进行 |
| `PingService` | 实现此接口，协议 `/ipfs/ping/1.0.0` |
| `IdentifyService` | 实现此接口，协议 `/ipfs/id/1.0.0` |
| `DhtService` | 实现此接口，协议 `/kad/1.0.0` |
