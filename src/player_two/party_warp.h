#pragma once

// Brings player 2 back to the party through the game's own follow warp (RE-p2-party-warp.md §1, §3, §9;
// RE-p2-chocobo-warp.md §2 and §4 for the ridden chocobo).
//
// An AI follower's follow state (EEndAIState FOLLOW) holds the game's party warp: a forced request (vtable +0x270)
// followed by the teleport update (+0x298), which runs the follow EQS for a spot near the leader, fades the follower
// out, warps it there and fades it back in. While player 2 drives the member, her AI controller is kept (character
// +0x588) with its follow state (the PLAYER variant), but its tick is gated off, so nothing calls the update. While
// player 2 rides their own chocobo, the same holds for the chocobo's AI and its follow state (a chocobo variant, the
// same force / query / finish-now functions): the game brings a far member chocobo back this way, its rider seated and
// carried by attachment. This module makes the forced request and calls the query and the update each tick until the
// game clears the request, exactly the calls the gated AI tick would make; the spot, fade, facing, warp and cooldown
// are all the game's. If the request has not finished within kFinishAfterSeconds, the game's own "finish now" (+0x278)
// completes it at the follow target, as the game does after a fast travel's wait. Game thread, outside every player-2
// scope (a follow state run as the party leader clears its leader; a chocobo's would follow itself).

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::party_warp {

/// Whose follow state is driven: player 2's member on foot, or the chocobo player 2 rides (the member seated on it).
enum class Subject
{
    Member,
    Chocobo,
};

/// Checks the PLAYER follow vtable's slots (required), the chocobo variants' (optional) and the two guards'
/// functions; logged either way.
bool Install(const platform::GameImage& image) noexcept;

/// Starts the forced warp for `pawn` (the member, or the chocobo she rides) if every guard holds (logs which one did
/// not). `attached`: the member's root component is attached to something (a ride; refused for Member only).
/// `splitShown`: the split is on screen (no menu, map or cutscene owns it). Game thread.
bool Start(void* pawn, Subject subject, bool attached, bool splitShown) noexcept;

/// Each engine tick: drives a started warp until the game clears it. `member`: player 2's member (null when none);
/// `chocobo`: the chocobo player 2 rides seated (null when not riding). Game thread.
void Tick(void* member, const void* chocobo, bool splitShown, float seconds) noexcept;

/// A warp is being driven.
bool Busy() noexcept;

/// The pawn's retained AI's current state, if it is a checked follow state for `subject` (null otherwise).
const void* FollowStateOf(const void* pawn, Subject subject) noexcept;

/// The pawn's retained AI for a log line: the controller and its pause mask, its state object and type, and whether
/// that state is a checked follow variant (then its teleport range and whether a forced warp is pending).
std::wstring AiText(const void* pawn, Subject subject) noexcept;

}  // namespace lss::player_two::party_warp
