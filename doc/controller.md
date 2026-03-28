# Controller

## 功能描述

`Controller` 是策略层接口，将连接管理决策与传输机制分离。`Swarm` 将每个 `SwarmEvent` 推送给 Controller，Controller 积累 `Action`，`Swarm` 在每次 `poll_once()` 时提取并执行这些 Action。

通过替换 Controller 实现，可以在不修改 `Swarm` 的前提下改变节点的连接行为策略（如自动重连、主动发现、带宽限制等）。

## 接口定义

### Controller（抽象基类）

```cpp
class Controller {
public:
    virtual ~Controller() = default;

    // Swarm 产生事件时调用
    virtual void on_swarm_event(const SwarmEvent& event) = 0;

    // Swarm::poll_once() 时调用，用于定时驱动策略逻辑
    virtual void on_timer_tick() = 0;

    // Swarm 提取 Action 列表（提取后清空队列）
    virtual std::vector<Action> drain_actions() = 0;
};
```

### DefaultController（内置空实现）

```cpp
class DefaultController : public Controller {
public:
    void on_swarm_event(const SwarmEvent& event) override;  // 无操作
    void on_timer_tick()                         override;  // 无操作
    std::vector<Action> drain_actions()          override;  // 返回空列表
};
```

默认 Controller 是纯空实现，允许在不需要自动策略时使用 `Node`/`Swarm`。

## Action 类型

```cpp
struct Action {
    enum class Type {
        DialAddr,         // 拨号到指定地址
        DialPeer,         // 拨号到指定 PeerId（需 PeerStore 有地址）
        CloseConnection,  // 关闭指定连接
        OpenStream,       // 在指定连接上打开流
        SendPing,         // 触发 Ping
    };
    Type                        type;
    std::optional<Multiaddr>    addr;
    std::optional<PeerId>       peer_id;
    std::optional<ConnectionId> connection_id;
    std::optional<ProtocolId>   protocol_id;
};
```

字段按 `type` 按需填充：
- `DialAddr`：填 `addr`
- `DialPeer`：填 `peer_id`
- `CloseConnection`：填 `connection_id`
- `OpenStream`：填 `connection_id` + `protocol_id`

## 事件-动作流转

```
Swarm::poll_once()
    ├─ controller_.on_timer_tick()
    └─ for each action in controller_.drain_actions():
         └─ Swarm::apply_action(action)
              ├─ DialAddr    → Swarm::dial_addr()
              ├─ DialPeer    → Swarm::dial_peer()
              ├─ CloseConn   → connection->close()
              └─ OpenStream  → Swarm::open_stream()

Swarm::dispatch_event(event)
    └─ controller_.on_swarm_event(event)
```

## 与其他模块的关系

| 关系 | 说明 |
|------|------|
| 由 `Swarm` 持有 | 默认为 `DefaultController`，可通过 `set_controller()` 替换 |
| 消费 `SwarmEvent` | 对事件做出策略判断 |
| 产出 `Action` | Swarm 读取并执行 |
| 不直接访问网络 | 所有网络操作通过 Swarm 执行，Controller 只做决策 |
