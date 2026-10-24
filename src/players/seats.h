#pragma once

// The seats: the further human players the mod adds (Documentation/plan/seats.md). Seat 0 is player 1, the game's
// own player; seat 1 is player 2, seat 2 player 3. A seat's number is at once its controller id, its pad (XInput user
// `seat`) and its slot of the party order after the field leader's (seat 1 the 2nd slot, seat 2 the 3rd). Its column
// (its view's place on screen) is its local player's index in the engine's list, which a join appends to and a leave
// closes up: with player 2 out, player 3 is in column 1. Nothing may take a list index for a seat. Pure constants and
// names, no game.

namespace lss::players {

/// Player 1 and the seats: the per-seat records are sized by it and indexed by the seat (index 0 is player 1's where
/// a record has one). Three, the engine's most split-screen views.
inline constexpr int kMaxPlayers = 3;

/// The further players built and wired: seats 1..kSeatsBuilt. Seat 2 is wired at the plan's step 6.
inline constexpr int kSeatsBuilt = 1;
static_assert(kSeatsBuilt >= 1 && kSeatsBuilt < kMaxPlayers, "the seats are numbered within the players");

/// Player 2's seat. A module not yet per seat names it, so every place still fixed to player 2 is found by this name
/// (plan/seats.md step 3).
inline constexpr int kPlayerTwoSeat = 1;

/// "player 2" for seat 1: the name a seat's log lines use, so three players read apart in the log.
constexpr const wchar_t* SeatName(int seat) noexcept
{
    constexpr const wchar_t* kNames[kMaxPlayers] = {L"player 1", L"player 2", L"player 3"};
    return seat >= 0 && seat < kMaxPlayers ? kNames[seat] : L"a player";
}

/// A seat has room in the per-seat records (1..kMaxPlayers-1), built or not.
constexpr bool SeatIndex(int seat) noexcept
{
    return seat >= 1 && seat < kMaxPlayers;
}

/// A seat is one of the built ones.
constexpr bool SeatBuilt(int seat) noexcept
{
    return seat >= 1 && seat <= kSeatsBuilt;
}

/// The mirror views' controller ids (views/mirror_player.h: a local player of the mod's that shows player 1's camera
/// in a column no player has), at most one per column beside player 1's. Never a seat's id: a view draws the HUD of
/// the seat its controller id names (hud/passes/second_image.h). And past XInput's four users (0 to 3): the engine
/// routes pad N to controller id N, so no pad ever reaches a mirror's controller.
inline constexpr int kXInputUsers   = 4;
inline constexpr int kFirstMirrorId = kXInputUsers;
static_assert(kFirstMirrorId >= kMaxPlayers, "a mirror's id is never a seat's");
inline constexpr int kMirrorIds     = kMaxPlayers - 1;

/// A mirror view's controller id.
constexpr bool IsMirrorId(int id) noexcept
{
    return id >= kFirstMirrorId && id < kFirstMirrorId + kMirrorIds;
}

}  // namespace lss::players
