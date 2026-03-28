# DhtService

## 功能描述

`DhtService` 实现基于 Kademlia 算法的分布式哈希表协议（`/kad/1.0.0`）。主要功能：

1. **节点发现**（`find_peer()`）：在网络中查找距目标 PeerId 最近的节点集合。
2. **内容声明**（`provide()`）：向网络声明本节点持有某个 key 对应的内容。
3. **路由维护**：通过 `on_inbound_stream()` / `on_tick()` 维护路由表，处理远端的 FIND_NODE 请求。

## 接口定义

```cpp
class DhtService : public ProtocolHandler {
public:
    static constexpr std::string_view kProtocolId = "/kad/1.0.0";

    ProtocolId protocol_id() const override;

    // 处理对端发来的 DHT 请求（FIND_NODE / GET_PROVIDERS 等）
    void on_inbound_stream(StreamHandle stream) override;

    // 发起本端的 DHT 请求
    void on_outbound_stream_ready(StreamHandle stream) override;

    // 定时维护：刷新桶、重新发布等
    void on_tick() override;

    // 在网络中查找距 target 最近的节点
    // callback 接收找到的 PeerId 列表（可能为空）
    void find_peer(const PeerId& target,
                   std::function<void(std::vector<PeerId>)> callback);

    // 声明本节点为 key 的提供者
    void provide(const PeerId& key);
};
```

## Kademlia 查找过程

```
find_peer(target)
    └─ 从 RoutingTable 取 k 个最近已知节点
         └─ 并发向这些节点发送 FIND_NODE 请求
              └─ 收集响应，更新候选集
                   └─ 迭代直到无更近节点或达到最大轮次
                        └─ callback(结果集)
```

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 实现 `ProtocolHandler` | 通过 `Swarm::register_handler()` 注册 |
| 由 `Node` 持有 | `Node::dht()` 返回其引用 |
| 持有 `RoutingTable` | 维护 Kademlia 路由表 |
| 读写 `PeerStore` | 发现新节点后记录其地址 |
| 使用 `MuxedStream` | 每次 DHT 查询在独立流上进行 |
| 与 `IdentifyService` 协作 | Identify 完成后 DhtService 将新 peer 加入路由表 |
