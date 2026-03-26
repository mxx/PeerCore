#include "../include/peercore/controller.hpp"

namespace peercore {

void DefaultController::on_swarm_event(const SwarmEvent& /*event*/) {
    // Override to implement reactive policy
}

void DefaultController::on_timer_tick() {
    // Override to implement periodic policy (e.g. redial peers)
}

std::vector<Action> DefaultController::drain_actions() {
    return std::exchange(pending_, {});
}

}  // namespace peercore
