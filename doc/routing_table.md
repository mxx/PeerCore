# RoutingTable

## 功能描述

`RoutingTable` 实现 Kademlia 分布式哈希表的路由表结构，用于在 DHT 中定位离目标 `PeerId` 最近的已知节点集合。本地节点以自身 `PeerId` 为基准，按 XOR 距离组织已知 peer。

当前实现使用平坦列表作为最小可用实现，k-bucket 结构预留为后续优化点。

## 接口定义

```cpp
class RoutingTable {
public:
    // k: 每个桶的最大容量，默认遵循 Kademlia 标准值 20
    explicit RoutingTable(const PeerId& local_id, size_t k = 20);

    void insert(const PeerId& peer);
    void remove(const PeerId& peer);

    // 返回距 target 最近的至多 k 个 peer，按 XOR 距离排序
    std::vector<PeerId> closest_peers(const PeerId& target, size_t k) const;

    size_t size() const;
};
```

## Kademlia 距离计算

节点 A 与 B 的距离定义为其 `PeerId` 字节数组的逐位 XOR 值。`closest_peers()` 按此距离升序排序返回结果。

`leading_zeros(a, b)` 计算 XOR 结果的前导零位数，用于确定 k-bucket 槽位（当前实现中作为排序依据）。

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 由 `DhtService` 持有 | DHT 服务内部维护路由表，查询时调用 `closest_peers()` |
| 依赖 `PeerId` | 仅操作 PeerId，不涉及地址，地址由 `PeerStore` 管理 |
| 独立于传输层 | 无网络 I/O，纯数据结构 |

## 当前限制与演进路径

| 现状 | 规划 |
|------|------|
| 平坦 `vector<PeerId>` | 替换为 256 个 k-bucket（按前导零位数分槽） |
| 无淘汰策略 | 实现 LRU 淘汰 + 活跃度检测 |
| 无持久化 | 可选磁盘持久化 |
