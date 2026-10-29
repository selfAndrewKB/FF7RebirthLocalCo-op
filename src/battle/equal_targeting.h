#pragma once

// Condition enemies favour both players equally (RE-p2-enemy-target-bias.md §5.1, §9, §10).
//
// The story bosses, summon fights and VR Sephiroth choose targets with the Condition selector, whose lists weight "the
// UCPC" (the user-controlled player character) 2:1 to 8:2 over the AI members. The game's UCPC is the party leader's
// type, player 1's character, so in co-op these enemies chase player 1. For each of their selects this answers the
// game's own two questions with one player, a uniform coin among the players (player 1 and each seat playing a member)
// whose characters could be targeted (battle/ucpc_choice.h):
//   - "which player type is the UCPC": the select's one read of it (UEndPartyAPI_GetPartyLeaderType, called once by
//     its candidate build) is redirected to the answer;
//   - "is this character the UCPC": IsUCPC_Core, which the select's ContinuousLimit history asks, answers yes for that
//     type only (scopes::leader_answer::ExchangeThreadOnly).
// Whether a player's character could be targeted is the game's own rule of the same select: its character for the
// type, IsValidTarget for this enemy, and the selector's excluded types. Both answers are held on the select's own
// thread for that select only: decisions run on worker threads as well as the game thread, and nothing is written to
// the party manager. Without a seat's member every select keeps the game's own answer. Every player's type is one
// word Tick writes on the game thread; the selects read it on any thread.
//
// Not touched: Hate and None enemies (no player term), lock groups, the opening target at battle start, and the
// other player-1 terms listed in RE §10.11.
//
// The Condition select's vtable slot is replaced here: the wrapper runs the game's select with the answer held for
// that call, then tells the one observer (battle/target_census, a dev tool) what it answered.

#include "battle/ucpc_choice.h"
#include "players/seats.h"

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::equal_targeting {

/// Places the leader-type redirect (logged). Needs leader_answer's slot. Call from on_unreal_init,
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
    ucpc_choice::Choice choice;        // choice.player: 0 player 1, s seat s
    std::uint8_t        type = 16;     // the player type answered as the UCPC (Untouched: not used)
    bool                read = false;  // the select built its candidates and read the answer
};

/// What one Condition select reported, after the game's select ran: the selector, its reason and out arguments (the
/// out holds the picked UObject*), the game's result (its low byte) and the answer held for it. Any thread, as the
/// selects run on worker threads too.
using SelectObserver = void (*)(void* selector, std::uint64_t reason, std::uint64_t out, std::uint64_t result,
                                const Answer& answer) noexcept;

/// The one observer of the Condition selects (null: none). Set before any battle and cleared at unload.
void SetSelectObserver(SelectObserver observer) noexcept;

/// A player type no player holds.
inline constexpr std::uint8_t kNoPlayerType = 16;

/// Every player's player type as Tick last saw them, one frame's set: player 1's (the leader's) at index 0, each
/// seat's member's at its seat (kNoPlayerType for a seat without a member, or whose member's type is the leader's).
/// Empty while no seat has a member (or before Install). Any thread.
struct PlayerTypes
{
    std::uint8_t type[players::kMaxPlayers] = {kNoPlayerType, kNoPlayerType, kNoPlayerType};

    /// `player` (0 player 1, s seat s) has a type this frame.
    bool Has(int player) const noexcept
    {
        return player >= 0 && player < players::kMaxPlayers && type[player] != kNoPlayerType;
    }
};
static_assert(players::kMaxPlayers == 3, "PlayerTypes' initialiser names every player");
std::optional<PlayerTypes> Types() noexcept;

/// Who an enemy with target selector `selector` counts as the player, by the rule of a Condition select: each player's
/// character must be a candidate for that selector, a uniform coin among them when two or more are. For
/// battle/target_coin's coin per enemy action. Untouched without a seat's member. Any thread.
Answer ChooseFor(void* selector) noexcept;

/// Whether `seat`'s member is still a candidate for `selector` (the same rule). Any thread.
bool SeatEligible(int seat, void* selector) noexcept;

}  // namespace lss::battle::equal_targeting
