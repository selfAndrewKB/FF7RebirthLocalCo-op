#pragma once

// A seat takes a party member, and gives it back.
//
// Taking: the game's own per-controller switch, AEndPlayerController::SwitchLeaderInternal, called
// for the seat's controller with a party member nobody plays. It needs the seat's trigger actor
// (EnsureEndPlayerTrigger creates it), and it writes two bytes to "the primary player's"
// controller, so it runs with the primary controller scoped to the seat
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

/// Why the seat has no member to take now (party_lease.h).
using SlotWait = session::SlotWait;

struct SlotChoice
{
    void*          member  = nullptr;  // the member to take; null while waiting
    SlotWait       wait    = SlotWait::NoSlot;
    std::uint8_t   type    = 0xFF;     // the slot's player type (0xFF with no slot)
    std::int32_t   ordinal = -1;
    bool           standsIn = false;   // the stand-in mark's row: player 1's former character
    bool           fieldLeaderInstead = false;  // the seat's row is player 1's: the field leader's row instead
    Eligibility    verdict = Eligibility::NoCharacter;
    AiSnapshot     ai{};
    LinkRecordView link{};             // with LinkPartner: the game's link record that holds it
};

/// The member `seat`'s player plays: player 2 the 2nd slot of the game's party order, the row after the field
/// leader in the battle-member list the party panel shows and player 1's battle switch cycles
/// (research/RE-p2-party-order.md); player 3 the 3rd slot, the row after that (party_lease.h SeatSlotOrdinal). While player 1 is on the seat's
/// row (the battle leader option, a switch), the seat plays the field leader's character player 1 left
/// (RowOrFieldLeader); with `standIn` set (party_lease.h StandInFor: the game's leader change took the seat's
/// member), the seat plays that character instead while it is deployed and not player 1's. The member is taken
/// only when free by the battle switch's own member test and its AI is in the state the game hands players (FOLLOW,
/// BATTLE), not while it is a synergy partner the game has not given back yet (party_lease.h LinkPartnerBusy), and not
/// while its AI is inside a field action (a ladder climb; world/party_warp.h InFieldAction); otherwise the seat waits on
/// it and never falls through to the next slot. Logged when `log` is set.
SlotChoice ChooseSlot(int seat, void* playerOneController, std::uint8_t standIn, bool log = true) noexcept;

/// `character` is the field leader's (the character the game's leader goes home to at a fight's end). Guarded.
bool IsFieldLeader(const void* character) noexcept;

/// The game's leader is the field leader's character: player 1 is home, no stand-in can hold. Guarded.
bool LeaderHome() noexcept;

/// One of the game's link records (research/RE-p2-synergy-skills.md); every read guarded, so any
/// thread that is running the record (an empty view for null).
LinkRecordView ReadLinkRecord(const void* record) noexcept;

/// The first of the game's link records whose partner is `playerType` and has not ended there (a record stays until
/// all its members ended, and the battle teardown keeps one whose started member never ended). Guarded reads; false if
/// none, outside battle, or if the reads faulted. Game thread.
bool FindPartnerLink(std::uint8_t playerType, LinkRecordView& out) noexcept;

/// `member`'s AI: its active brain and that brain's state. Game thread.
AiSnapshot ReadAi(const void* member) noexcept;

/// Why `member` is no longer in a seat's reach in the party: now the party leader (a story change
/// hands it to player 1), not one of the deployed party any more, or away from the battle party.
/// Null while it is. Game thread.
const wchar_t* WhyNotInParty(const void* member) noexcept;

/// `member` still has its battle controller component (true when it cannot be read). Game thread.
bool HasBattleController(const void* member) noexcept;

inline constexpr const wchar_t* kWithoutBattleController = L"without its battle controller";

/// Why a seat may no longer play `member`: WhyNotInParty, or without its battle controller
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

/// For a script that needs `member` (a level's walk or lead, an event's walk, an animation): the game's own hand to
/// its AI, as its events hand player 1's leader. The player's controller keeps it as its leader, so the player's camera
/// keeps following it and their stick keeps turning it; no leader change is broadcast (research/RE-held-member-scripts.md).
/// False (nothing changed) when the call is unavailable or the member's AI is not valid. Game thread.
bool HandToAi(void* member) noexcept;

/// The game's possession of `member` by `seat`'s `controller` again, after a hand (HandToAi), inside the seat's
/// primary scope. False when the scope or the call is refused (logged). Game thread.
bool ReturnTo(int seat, void* member, void* controller) noexcept;

/// `character` is the game's leader now (player 1's character). Guarded.
bool IsGameLeader(const void* character) noexcept;

/// `character`'s actor type through its actor interface's getter (facts::kActorTypePlayer, facts::kActorTypeParty, ...),
/// or nullopt when it cannot be read.
std::optional<std::uint8_t> ActorType(void* character) noexcept;

/// `character`'s actor type set with the game's setter, which re-registers its per-type invisible walls. False when it
/// cannot be called or faulted.
bool SetActorType(void* character, std::uint8_t type) noexcept;

/// The lease fields for `member` and the seat's controller.
LeaseState ReadLease(const void* member, const void* seatController) noexcept;

/// The two bytes on player 1's controller the switch would write without the scope.
struct PlayerOneBytes
{
    std::uint8_t at897;
    std::uint8_t at8AB;
    bool operator==(const PlayerOneBytes&) const = default;
};
PlayerOneBytes ReadPlayerOneBytes(const void* playerOneController) noexcept;

}  // namespace lss::session::party_member
