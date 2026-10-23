#pragma once

// The control runner's stick holds on the running mod: a scripted hold is a per-pad stick override
// (player_two/input/pad_sticks.h) on the held player's controller id, so it takes a real pad's path. Needs per-pad
// sticks on and player 2 in (the override is per pad only then). Game thread only.

#include "control/runner.h"

#include <array>
#include <cstdint>

namespace lss::player_two {
class Session;
}

namespace lss::control {

class StickHolds final : public Holds
{
public:
    explicit StickHolds(player_two::Session& session) noexcept : session_(session) {}

    Outcome HoldStick(std::int32_t player, const float (&axes)[4]) override;
    void    ReleaseStick(std::int32_t player) override;
    bool    Held(std::int32_t player) const override;

    /// Clears every hold made during a join that has since ended (a leave by key, a map load), so a later player 2
    /// on the same pad never inherits it. Once per tick, after the session's tick and before the world's input runs.
    void DropEndedHolds() noexcept;

private:
    player_two::Session&                    session_;
    std::array<std::int32_t, kHoldPlayers>  heldIds_{-1, -1};  // per player: the controller id its hold is on
    std::array<std::uint32_t, kHoldPlayers> heldJoins_{};      // per player: the join its hold was made in
};

}  // namespace lss::control
