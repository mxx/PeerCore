# PeerStore

## 功能描述

`PeerStore` 是节点地址簿，负责存储已知 peer 的网络地址以及拨号成功/失败历史。`Swarm::dial_peer()` 依赖 `PeerStore` 将 `PeerId` 解析为可拨号的 `Multiaddr`。

## 接口定义

```cpp
class PeerStore {
public:
    // 为 peer 添加一个已知地址
    void add_addr(const PeerId& peer, const Multiaddr& addr);

    // 获取 peer 的所有已知地址（无则返回空列表）
    std::vector<Multiaddr> get_addrs(const PeerId& peer) const;

    // 记录拨号结果（用于未来地址排序/淘汰策略）
    void record_dial_success(const PeerId& peer);
    void record_dial_failure(const PeerId& peer);
};
```

## 内部数据结构

```cpp
struct Entry {
    PeerId peer;
    std::vector<Multiaddr> addrs;
    uint32_t dial_successes{0};
    uint32_t dial_failures{0};
};
std::vector<Entry> entries_;
```

当前实现使用线性扫描查找，适合节点数较少的场景。`dial_successes` / `dial_failures` 为未来实现地址优先级排序预留字段。

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 由 `Node` 持有 | `Node` 构造 `PeerStore` 并以引用形式传给 `Swarm` |
| 被 `Swarm` 使用 | `Swarm::dial_peer()` 调用 `get_addrs()` 解析地址 |
| 被 `IdentifyService` 写入 | 识别到 peer 的监听地址后调用 `add_addr()` |
| 被 `DhtService` 写入 | 发现新 peer 时记录地址 |
| 生命周期 | `Node` 析构前 `PeerStore` 必须存活（`Swarm` 持有其引用） |
