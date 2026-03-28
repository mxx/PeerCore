# MuxedStream / StreamHandle

## 功能描述

`MuxedStream` 是 Yamux 多路复用器上单条逻辑流的抽象接口，提供非阻塞读写、半关闭和重置操作。一条 TCP 连接上可以并发运行多条 `MuxedStream`，每条流承载一个协议会话。

`StreamHandle` 是 `shared_ptr<MuxedStream>` 的别名，是流在系统中流通的主要形式。

## 接口定义

```cpp
class MuxedStream {
public:
    virtual StreamId     id()            const = 0;
    virtual ConnectionId connection_id() const = 0;

    // 非阻塞读写，返回实际操作字节数
    virtual Result<size_t> try_read(MutableBytes buf)  = 0;
    virtual Result<size_t> try_write(ConstBytes data)  = 0;

    // 半关闭（发送 FIN）
    virtual Result<void> close_write() = 0;

    // 强制重置（发送 RST，类似 TCP RST）
    virtual Result<void> reset() = 0;

    virtual bool is_open() const = 0;

    // multistream-select 协商完成后返回协商的协议
    virtual std::optional<ProtocolId> negotiated_protocol() const = 0;
};

using StreamHandle = std::shared_ptr<MuxedStream>;
```

## 读写语义

- `try_read()` / `try_write()` 为**非阻塞**：若无数据/缓冲区满则返回 0 字节，不阻塞调用方。
- `close_write()` 发送 Yamux DATA 帧的 FIN 标志，表示本端不再写入，但仍可继续读取对端数据（半关闭）。
- `reset()` 发送 Yamux RST 帧，立即终止流的双向通信。

## 生命周期

```
流创建（outbound: request_open_stream / inbound: accept_inbound_stream）
    └─ multistream-select 协议协商
         └─ 协议处理器接管（on_outbound_stream_ready / on_inbound_stream）
              └─ 数据交换（try_read / try_write）
                   └─ close_write() 或 reset() 结束
```

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 由 `ConnectionSession` 创建 | `request_open_stream` / `accept_inbound_stream` 返回 `StreamHandle` |
| 由 `ProtocolHandler` 消费 | `on_inbound_stream` / `on_outbound_stream_ready` 接收 `StreamHandle` |
| 传递给 `PingService` | `ping(stream, callback)` 在流上执行 ping |
| 传递给 `IdentifyService` | `identify(stream, callback)` 在流上执行身份识别 |
| `ConnectionId` 关联 | 通过 `connection_id()` 可以追溯到所属连接 |
