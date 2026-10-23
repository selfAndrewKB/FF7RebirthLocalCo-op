#include "control/stick_holds.h"

#include "player_two/generation.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/session/session.h"

namespace lss::control {

namespace {

/// The controller id of player 1's or player 2's controller, or -1.
std::int32_t ControllerIdOfPlayer(std::int32_t player) noexcept
{
    void* controller =
        player == 1 ? player_two::generation::PlayerOneController() : player_two::generation::PlayerTwoController();
    return controller == nullptr ? -1 : player_two::pad_sticks::ControllerIdOf(controller);
}

}  // namespace

Outcome StickHolds::HoldStick(std::int32_t player, const float (&axes)[4])
{
    if (player < 1 || player > kHoldPlayers)
    {
        return {false, L"no such player"};
    }
    if (!player_two::pad_sticks::Enabled())
    {
        return {false, player_two::pad_sticks::SwitchedOn()
                           ? L"per-pad input is stood down (player 1 rides the buggy or the bronco)"
                           : L"per-pad sticks are off (run 'sticks per-pad' first)"};
    }
    if (!player_two::generation::Active())
    {
        return {false, L"player 2 is not in, so no stick is scoped per pad"};
    }
    const std::int32_t id = ControllerIdOfPlayer(player);
    if (id < 0)
    {
        return {false, L"that player's controller id cannot be read"};
    }
    ReleaseStick(player);  // a player's earlier hold may be on another id
    const player_two::Sticks sticks{axes[0], axes[1], axes[2], axes[3]};
    player_two::pad_sticks::SetStickOverride(id, &sticks);
    heldIds_[static_cast<std::size_t>(player - 1)]   = id;
    heldJoins_[static_cast<std::size_t>(player - 1)] = session_.JoinCount();
    return {true, L"controller id " + std::to_wstring(id)};
}

void StickHolds::ReleaseStick(std::int32_t player)
{
    if (player < 1 || player > kHoldPlayers)
    {
        return;
    }
    // The id recorded at the hold, not a fresh read: after a leave the controller is gone.
    auto& id = heldIds_[static_cast<std::size_t>(player - 1)];
    if (id >= 0)
    {
        player_two::pad_sticks::SetStickOverride(id, nullptr);
        id = -1;
    }
}

bool StickHolds::Held(std::int32_t player) const
{
    return player >= 1 && player <= kHoldPlayers && heldIds_[static_cast<std::size_t>(player - 1)] >= 0;
}

void StickHolds::DropEndedHolds() noexcept
{
    const bool active = player_two::generation::Active();
    for (std::size_t i = 0; i < heldIds_.size(); ++i)
    {
        if (heldIds_[i] >= 0 && (!active || heldJoins_[i] != session_.JoinCount()))
        {
            ReleaseStick(static_cast<std::int32_t>(i + 1));
        }
    }
}

}  // namespace lss::control
