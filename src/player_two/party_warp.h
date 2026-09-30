#pragma once

// Brings player 2's member back to the party through the game's own follow warp (RE-p2-party-warp.md §1, §3, §9).
//
// An AI follower's follow state (EEndAIState FOLLOW, the PLAYER variant) holds the game's party warp: a forced
// request (vtable +0x270) followed by the teleport update (+0x298), which runs the follow EQS for a spot near the
// leader, fades the member out, warps her there facing player 1's camera and fades her back in. While player 2
// drives the member, her AI controller is kept (character +0x588) with its follow state, but its tick is gated
// off, so nothing calls the update. This module makes the forced request and calls the update each tick until
// the game clears the request, exactly the two calls the gated AI tick would make; the spot, fade, facing, warp
// and cooldown are all the game's. If the request has not finished within kFinishAfterSeconds, the game's own
// "finish now" (+0x278) completes it at the follow target, as the game does after a fast travel's wait.
// Game thread, outside every player-2 scope (a follow state run as the party leader clears its leader).

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::party_warp {

/// Checks the PLAYER follow vtable's three slots and the two guards' functions; logged either way.
bool Install(const platform::GameImage& image) noexcept;

/// Starts the forced warp for `member` if every guard holds (logs which one did not). `attached`: her root
/// component is attached to something (a ride). `splitShown`: the split is on screen (no menu, map or
/// cutscene owns it). Game thread.
bool Start(void* member, bool attached, bool splitShown) noexcept;

/// Each engine tick: drives a started warp until the game clears it. Game thread.
void Tick(void* member, bool splitShown, float seconds) noexcept;

}  // namespace lss::player_two::party_warp
