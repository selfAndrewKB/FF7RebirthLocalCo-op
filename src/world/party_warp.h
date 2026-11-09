#pragma once

// Brings a seat's player back to the party through the game's own follow warp (RE-p2-party-warp.md §1, §3, §9;
// RE-p2-chocobo-warp.md §2 and §4 for the ridden chocobo).
//
// An AI follower's follow state (EEndAIState FOLLOW) holds the game's party warp: a forced request (vtable +0x270)
// followed by the teleport update (+0x298), which runs the follow EQS for a spot near the leader, fades the follower
// out, warps it there and fades it back in. While a seat's player drives the member, its AI controller stays (character
// +0x588) with its follow state (the PLAYER variant), but its tick is gated off, so nothing calls the update. While
// that player rides their own chocobo, the same holds for the chocobo's AI and its follow state (a chocobo variant, the
// same force / query / finish-now functions): the game brings a far member chocobo back this way, its rider seated and
// carried by attachment. This module makes the forced request and calls the query and the update each tick until the
// game clears the request, exactly the calls the gated AI tick would make; the spot, fade, facing, warp and cooldown
// are all the game's. If the request has not finished within kFinishAfterSeconds, the game's own "finish now" (+0x278)
// completes it at the follow target, as the game does after a fast travel's wait. Game thread, outside every seat's
// scope (a follow state run as the party leader clears its leader; a chocobo's would follow itself).
//
// Per seat (players/seats.h): any seat's member or ridden chocobo is a subject. One warp is driven
// at a time and owned by the seat that started it: another seat's Start is refused (logged) while it runs; nav_strand
// holds that seat's request until the warp is over. Whether the game takes two forced follow warps at once is an
// open question.

#include "players/seats.h"

#include <array>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::world::party_warp {

/// Whose follow state is driven: a seat's member on foot, or the chocobo the seat's player rides (the member seated on
/// it).
enum class Subject
{
    Member,
    Chocobo,
};

/// A pawn per seat (index 0 unused; null: none).
using SeatPawns = std::array<const void*, players::kMaxPlayers>;

/// Checks the PLAYER follow vtable's slots (required), the chocobo variants' (optional) and the two guards'
/// functions; logged either way.
bool Install(const platform::GameImage& image) noexcept;

/// Starts `seat`'s forced warp for `pawn` (the member, or the chocobo she rides) if every guard holds and no warp is
/// being driven (logs which one did not). The guards begin with the follow update's own that a forced request skips
/// (RE-warp-gates.md): the field up, no scene, talk or event holding it, neither player 1's character nor the seat's
/// member in a field action (a ladder, a climb, a crawl, the grappling hook). `attached`: the member's root component is attached to something (a ride;
/// refused for Member only). `splitShown`: the split is on screen (no menu, map or cutscene owns it). Game thread.
bool Start(int seat, void* pawn, Subject subject, bool attached, bool splitShown) noexcept;

/// Each engine tick: drives a started warp until the game clears it. By seat, `members`: each seat's member (null
/// when none); `chocobos`: the chocobo each seat's player rides seated (null when not riding). Game thread.
void Tick(const SeatPawns& members, const SeatPawns& chocobos, bool splitShown, float seconds) noexcept;

/// A warp is being driven.
bool Busy() noexcept;

/// The seat whose warp is being driven, or -1.
int BusySeat() noexcept;

/// `pawn` is a party character in a field action (a ladder, a climb, a crawl, the grappling hook) or another field
/// controller state, by the game's own test (the follow update's gate); true when the test faults. False for a pawn that
/// is no party character, and when the test was not found at install. Game thread.
bool InFieldAction(const void* pawn) noexcept;

/// The pawn's retained AI for a log line: the controller and its pause mask, its state object and type, and whether
/// that state is a checked follow variant (then its teleport range and whether a forced warp is pending).
std::wstring AiText(const void* pawn, Subject subject) noexcept;

}  // namespace lss::world::party_warp
