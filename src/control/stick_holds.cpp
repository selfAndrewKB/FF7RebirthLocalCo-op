#include "control/stick_holds.h"

#include "input/pad_sticks.h"
#include "players/generation.h"
#include "players/seats.h"
#include "session/session.h"

namespace lss::control {

namespace {

/// The controller id of a player's controller (player 1 is seat 0), or -1.
std::int32_t ControllerIdOfPlayer(std::int32_t player) noexcept
{
    void* controller = players::generation::ControllerOf(player - 1);
    return controller == nullptr ? -1 : input::pad_sticks::ControllerIdOf(controller);
}

/// The seat whose generation a player's hold needs, and whose join it lasts for: player 2's or 3's, its own; player
/// 1's, the first seat whose player is in (the override is per pad only with a seat in), 0 if none is.
int SeatOfHold(std::int32_t player) noexcept
{
    if (player != 1)
    {
        return player - 1;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat))
        {
            return seat;
        }
    }
    return 0;
}

}  // namespace

Outcome StickHolds::HoldStick(std::int32_t player, const float (&axes)[4])
{
    if (player < 1 || player > kHoldPlayers)
    {
        return {false, L"no such player"};
    }
    if (!input::pad_sticks::Enabled())
    {
        return {false, input::pad_sticks::SwitchedOn()
                           ? L"per-pad input is stood down (player 1 rides the buggy or the bronco)"
                           : L"per-pad sticks are off (run 'sticks per-pad' first)"};
    }
    const int seat = SeatOfHold(player);
    if (!players::generation::Active(seat))
    {
        return {false, player == 1 ? L"no other player is in, so no stick is scoped per pad" : L"that player is not in"};
    }
    const std::int32_t id = ControllerIdOfPlayer(player);
    if (id < 0)
    {
        return {false, L"that player's controller id cannot be read"};
    }
    ReleaseStick(player);  // a player's earlier hold may be on another id
    const input::Sticks sticks{axes[0], axes[1], axes[2], axes[3]};
    input::pad_sticks::SetStickOverride(id, &sticks);
    heldIds_[static_cast<std::size_t>(player - 1)]   = id;
    heldSeats_[static_cast<std::size_t>(player - 1)] = seat;
    heldJoins_[static_cast<std::size_t>(player - 1)] = session_.JoinCount(seat);
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
        input::pad_sticks::SetStickOverride(id, nullptr);
        id = -1;
    }
}

bool StickHolds::Held(std::int32_t player) const
{
    return player >= 1 && player <= kHoldPlayers && heldIds_[static_cast<std::size_t>(player - 1)] >= 0;
}

void StickHolds::DropEndedHolds() noexcept
{
    for (std::size_t i = 0; i < heldIds_.size(); ++i)
    {
        const std::int32_t player = static_cast<std::int32_t>(i + 1);
        const int          seat   = heldSeats_[i];
        if (heldIds_[i] >= 0 && (!players::generation::Active(seat) || heldJoins_[i] != session_.JoinCount(seat)))
        {
            ReleaseStick(player);
        }
    }
}

}  // namespace lss::control
