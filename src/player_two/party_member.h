#pragma once

// Player 2 takes a party member, and gives it back.
//
// Taking: the game's own per-controller switch, AEndPlayerController::SwitchLeaderInternal, called
// for player 2's controller with a party member nobody plays. It needs player 2's trigger actor
// (EnsureEndPlayerTrigger creates it), and it writes two bytes to "the primary player's"
// controller, so it runs with the primary controller scoped to player 2
// (player_two/primary_controller.h); otherwise player 1's controller would take those writes.
// The switch hands control over asynchronously, so success is read from the lease fields over the
// following ticks (player_two/party_lease.h), never from the call.
//
// The switch only moves control while the field/battle gate it checks internally is open; with it
// closed it would still publish the leader, a half state. So taking happens only in the field. A
// give-back the game's party changes need in battle is completed by the game's own battle rule
// (ReturnToAi).
//
// Game thread only, outside Draw.

#include "player_two/party_lease.h"

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::party_member {

/// Finds every function this needs. False (logged) if any check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// The switch's own gate: in the field, or not in battle.
bool FieldGateOpen() noexcept;

/// The first eligible member of the deployed party (the battle-member list player 1's battle switch
/// cycles), by that switch's own member test, never player 1's character or the field or default
/// battle leader's slot (null if none). Each member's verdict is logged when `log` is set.
void* Choose(void* playerOneController, bool log = true) noexcept;

/// Why `member` is no longer in player 2's reach in the party: now the party leader (a story change
/// hands it to player 1), not one of the deployed party any more, or away from the battle party.
/// Null while it is. Game thread.
const wchar_t* WhyNotInParty(const void* member) noexcept;

/// `member` still has its battle controller component (true when it cannot be read). Game thread.
bool HasBattleController(const void* member) noexcept;

inline constexpr const wchar_t* kWithoutBattleController = L"without its battle controller";

/// Why player 2 may no longer play `member`: WhyNotInParty, or without its battle controller
/// component. A downed or petrified member stays playable, as player 1 stays on a downed leader.
/// Null while it may. Game thread.
const wchar_t* WhyNotPlayable(const void* member) noexcept;

/// The party roster's player type of `character` (null if it is not on the field lane's roster).
std::optional<std::uint8_t> PlayerTypeOf(const void* character) noexcept;

/// Creates player 2's trigger actor if needed and hands `member` to player 2's controller.
/// Returns false if the trigger could not be made, the primary controller could not be scoped, or
/// the engine raised an exception; the caller then waits for nothing.
bool Take(void* playerTwoController, void* member) noexcept;

/// Hands player 2's party member back to its AI. The trigger actor must still exist. With the gate
/// closed (in battle) the switch publishes the release but moves no control; ReturnToAi does that.
bool GiveBack(void* playerTwoController) noexcept;

/// The game's own battle rule, no player controller possesses a member in battle
/// (RE-p2-battle-handover.md §4.2): AEndCharacter::SwitchAIControl_Async(member, 1, nullptr), as the
/// battle start does for the field leader. Player 2's controller is unpossessed and the member's
/// saved AI controller possesses it. False (nothing called) if the member has no saved AI controller.
bool ReturnToAi(void* member) noexcept;

/// The lease fields for `member` and player 2's controller.
LeaseState ReadLease(const void* member, const void* playerTwoController) noexcept;

/// The two bytes on player 1's controller the switch would write without the scope.
struct PlayerOneBytes
{
    std::uint8_t at897;
    std::uint8_t at8AB;
    bool operator==(const PlayerOneBytes&) const = default;
};
PlayerOneBytes ReadPlayerOneBytes(const void* playerOneController) noexcept;

}  // namespace lss::player_two::party_member
