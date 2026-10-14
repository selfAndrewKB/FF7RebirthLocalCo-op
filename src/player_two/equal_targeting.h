#pragma once

// Condition enemies favour both players equally (RE-p2-enemy-target-bias.md §5.1, §9, §10).
//
// The story bosses, summon fights and VR Sephiroth choose targets with the Condition selector, whose lists weight "the
// UCPC" (the user-controlled player character) 2:1 to 8:2 over the AI members. The game's UCPC is the party leader's
// type, player 1's character, so in co-op these enemies chase player 1. For each of their selects this answers the
// game's own two questions with one player, a coin between the two when both could be targeted
// (player_two/ucpc_choice.h):
//   - "which player type is the UCPC": the select's one read of it (UEndPartyAPI_GetPartyLeaderType, called once by
//     its candidate build) is redirected to the answer;
//   - "is this character the UCPC": IsUCPC_Core, which the select's ContinuousLimit history asks, answers yes for that
//     type only (member_movement::ThreadOnlyLeader).
// Whether a player's character could be targeted is the game's own rule of the same select: its character for the
// type, IsValidTarget for this enemy, and the selector's excluded types. Both answers are held on the select's own
// thread for that select only: decisions run on worker threads as well as the game thread, and nothing is written to
// the party manager. Without player 2 (or without a member) every select keeps the game's own answer.
//
// Not touched: Hate and None enemies (no player term), lock groups, the opening target at battle start, and the
// other player-1 terms listed in RE §10.11.
//
// The select's vtable slot belongs to player_two/target_census, whose wrapper runs the game's select through RunSelect
// and logs the answers.

#include "player_two/ucpc_choice.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::equal_targeting {

/// Places the leader-type redirect (logged). Needs member_movement's leader-test slot. Call from on_unreal_init,
/// before any world exists: on the research build the call crosses a 16-byte block, which is safe to change only
/// while no thread can be running it.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// The redirect is in place and answering (between Install and Remove).
bool Installed() noexcept;

/// Game thread, before the world tick, where no leader scope is held: both players' types for the selects to read.
void Tick() noexcept;

/// One select's answer.
struct Answer
{
    ucpc_choice::Choice choice;
    std::uint8_t        type = 16;     // the player type answered as the UCPC (Untouched: not used)
    bool                read = false;  // the select built its candidates and read the answer
};

/// The Condition select's signature: bool(selector, u8 reason, UObject** out), passed through as four registers.
using SelectFn = std::uint64_t (*)(std::uint64_t selector, std::uint64_t reason, std::uint64_t out, std::uint64_t d);

/// Runs the game's Condition `select` with who it counts as the UCPC answered on this thread, for that call only;
/// `answer` reports it. The answers are put back however the call leaves, also when a structured exception unwinds
/// through it. Any thread.
std::uint64_t RunSelect(SelectFn select, std::uint64_t selector, std::uint64_t reason, std::uint64_t out,
                        std::uint64_t d, Answer& answer) noexcept;

}  // namespace lss::player_two::equal_targeting
