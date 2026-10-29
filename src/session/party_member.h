#pragma once

// Player 2 takes a party member, and gives it back.
//
// Taking: the game's own per-controller switch, AEndPlayerController::SwitchLeaderInternal, called
// for player 2's controller with a party member nobody plays. It needs player 2's trigger actor
// (EnsureEndPlayerTrigger creates it), and it writes two bytes to "the primary player's"
// controller, so it runs with the primary controller scoped to player 2
// (scopes/primary_controller.h); otherwise player 1's controller would take those writes.
// The switch hands control over asynchronously, so success is read from the lease fields over the
// following ticks (session/party_lease.h), never from the call. It moves control in the field and
// in battle alike, as it does for player 1's battle leader switch.
//
// Game thread only, outside Draw.

#include "session/party_lease.h"

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::session::party_member {

/// Finds every function this needs. False (logged) if any check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// The game has started: its battle system and its option data exist (false at the title and early in
/// a load, when the party's objects cannot be relied on).
bool GameReady() noexcept;

/// Why player 2 has no member to take now (party_lease.h).
using SlotWait = session::SlotWait;

struct SlotChoice
{
    void*          member  = nullptr;  // the member to take; null while waiting
    SlotWait       wait    = SlotWait::NoSlot;
    std::uint8_t   type    = 0xFF;     // the 2nd slot's player type (0xFF with no slot)
    std::int32_t   ordinal = -1;
    Eligibility    verdict = Eligibility::NoCharacter;
    AiSnapshot     ai{};
    LinkRecordView link{};             // with LinkPartner: the game's link record that holds it
};

/// The member `seat`'s player plays: player 2 the 2nd slot of the game's party order, the row after the field
/// leader in the battle-member list the party panel shows and player 1's battle switch cycles, stepping past
/// player 1's character as that cycle does (RE-p2-party-order.md §5); player 3 the 3rd slot, the row after that
/// (party_lease.h SeatSlotOrdinal). It is taken only when free by the battle switch's own member test and its AI is
/// in the state the game hands players (FOLLOW, BATTLE), and not while it is a synergy partner the game has not
/// given back yet (party_lease.h LinkPartnerBusy); otherwise the seat waits on it and never falls through to the
/// next slot. Logged when `log` is set.
SlotChoice ChooseSlot(int seat, void* playerOneController, bool log = true) noexcept;

/// One of the game's link records (RE-p2-synergy-skills.md S-SYNTAKE §3, S-SYNSTUCK2 §1); every read guarded, so any
/// thread that is running the record (an empty view for null).
LinkRecordView ReadLinkRecord(const void* record) noexcept;

/// The first of the game's link records whose partner is `playerType` and has not ended there (a record stays until
/// all its members ended, and the battle teardown keeps one whose started member never ended). Guarded reads; false if
/// none, outside battle, or if the reads faulted. Game thread.
bool FindPartnerLink(std::uint8_t playerType, LinkRecordView& out) noexcept;

/// `member`'s AI: its active brain and that brain's state. Game thread.
AiSnapshot ReadAi(const void* member) noexcept;

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

/// Creates the trigger actor of `seat`'s controller if needed and hands `member` to it (the switch runs with the
/// primary controller scoped to the seat). Returns false if the trigger could not be made, the primary controller
/// could not be scoped, or the engine raised an exception; the caller then waits for nothing.
bool Take(int seat, void* controller, void* member) noexcept;

/// Hands `seat`'s party member back to its AI. The trigger actor must still exist.
bool GiveBack(int seat, void* controller) noexcept;

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

}  // namespace lss::session::party_member
