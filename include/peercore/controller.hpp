#pragma once

#include "events.hpp"

#include <vector>

namespace peercore {

// Strategy layer: reacts to SwarmEvents and produces Actions for Swarm to execute.
// Keeps policy logic separate from transport mechanics.
class Controller {
public:
    virtual ~Controller() = default;

    virtual void on_swarm_event(const SwarmEvent& event) = 0;
    virtual void on_timer_tick()                         = 0;

    // Swarm drains this queue each poll cycle
    virtual std::vector<Action> drain_actions() = 0;
};

// Default no-op controller (can be subclassed)
class DefaultController : public Controller {
public:
    void on_swarm_event(const SwarmEvent& event) override;
    void on_timer_tick()                         override;
    std::vector<Action> drain_actions()          override;

private:
    std::vector<Action> pending_;
};

}  // namespace peercore
