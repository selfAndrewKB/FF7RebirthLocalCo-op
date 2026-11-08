#pragma once

// The rules for lending a party member to a seat's controller, as pure functions over values
// read from the game (tested on the host: tests/party_lease_test.cpp).
//
// A field party member that nobody plays is driven by its own AI controller. Square's character
// keeps that ownership in four fields (the "lease"):
//   +0x578 the controller that asked for the character (null when nobody asked)
//   +0x580 the controller that drives it now
//   +0x588 the AI controller it returns to
//   +0x260 its Unreal controller (APawn::Controller)
// and the AI controller's own pawn field (+0x258) names the character back. The seat's switch
// (AEndPlayerController::SwitchLeaderInternal) moves +0x578/+0x580/+0x260 to the seat's
// controller and back; a character is only offered to it while every field says "AI-owned".

#include <cstdint>

namespace lss::session {

/// One party member, as read from the game just before choosing.
struct MemberSnapshot
{
    const void*  character         = nullptr;
    bool         isEndCharacter    = false;
    bool         isPlayerOnes      = false;  // the global leader, player 1's pawn or player 1's leader
    const void*  requested         = nullptr;  // +0x578
    const void*  effective         = nullptr;  // +0x580
    const void*  retainedAi        = nullptr;  // +0x588
    const void*  controller        = nullptr;  // +0x260
    const void*  aiPawn            = nullptr;  // the retained AI controller's +0x258
    std::uint8_t playerControlled  = 0;        // +0x625
    bool         aiControllerValid = false;    // the switch's own gate on the AI component
    bool         leaderSlot        = false;    // the field leader's or the default battle leader's type
    bool         battleReady       = true;     // passes player 1's battle switch's own member test
};

enum class Eligibility
{
    Eligible,
    NoCharacter,
    NotEndCharacter,
    PlayerOnes,
    LeaderSlot,    // the game hands this type back to player 1 (battle end, or battle start)
    Requested,     // another controller has asked for it
    NotAiOwned,    // +0x580, +0x588, +0x260 and the AI's pawn do not all agree
    PlayerDriven,  // +0x625 says a player drives it
    AiInvalid,         // the switch would publish the leader without moving control
    BattleUnavailable, // player 1's battle switch would not take it either (its state 6 or state id 22)
};

Eligibility Assess(const MemberSnapshot& member) noexcept;

const wchar_t* Describe(Eligibility eligibility) noexcept;

// ---- the member a seat plays: its slot of the party order (RE-p2-party-order.md §5) ----

/// The field leader's ordinal in the battle-member list, the party panel's top row (0 if absent).
std::int32_t LeaderOrdinal(const std::uint8_t* types, std::int32_t count, std::uint8_t fieldLeader) noexcept;

/// The ordinal `step` rows below `anchor` (step from 1), wrapping as the panel and the battle switch's
/// cycle do. `count` must be positive.
constexpr std::int32_t OrdinalAfter(std::int32_t anchor, std::int32_t step, std::int32_t count) noexcept
{
    return (anchor + step) % count;
}

/// The ordinal seat `seat` plays: of the rows below `anchor` (wrapping, the anchor's own last), those `skipped`
/// does not step past (an empty row, player 1's character), the `seat`-th. Player 2 (seat 1) plays the first, the
/// 2nd slot of the order; player 3 (seat 2) the second, the 3rd slot (the user's rule; plan/seats.md). A seat's
/// slot is its place in the order, whoever holds the rows before it. -1 if the party has no such row. `skipped` is
/// asked once per row, in order, and last of the chosen row (ChooseSlot keeps what it read there).
template <class Skipped>
constexpr std::int32_t SeatSlotOrdinal(std::int32_t anchor, std::int32_t count, std::int32_t seat, Skipped skipped)
{
    if (count <= 0 || seat < 1)
    {
        return -1;
    }
    std::int32_t passed = 0;
    for (std::int32_t step = 1; step <= count; ++step)
    {
        const std::int32_t ordinal = OrdinalAfter(anchor, step, count);
        if (!skipped(ordinal) && ++passed == seat)
        {
            return ordinal;
        }
    }
    return -1;
}

// ---- the member's AI at the hand-over (RE-p2-member-handover.md §3) ----

/// A member's AI as the hand-over needs it: its active brain (0 is the base brain; a story script
/// pushes others) and that brain's state (EEndAIState, facts::AiState).
struct AiSnapshot
{
    bool         read       = false;
    std::uint8_t brainIndex = 0xFF;
    std::uint8_t state      = 0xFF;
};

enum class AiReadiness
{
    Ready,        // the base brain's FOLLOW, or BATTLE: the states the game hands a player
    Unread,
    ScriptBrain,  // a story script's brain holds the member (a scene, a guide, a wait)
    NotFollow,    // the base brain, in another state
};

/// Whether a seat may take a member whose AI is `ai`. Only FOLLOW's hand-over cleanup removes the
/// AI's catch-up modifiers, so nothing else is taken.
AiReadiness AssessAi(const AiSnapshot& ai) noexcept;

const wchar_t* Describe(AiReadiness readiness) noexcept;

/// A held member whose AI a story script puts into LEAD is given back: LEAD walks a route the story
/// waits on, and does not run while a player holds the member.
bool AiTakesBack(const AiSnapshot& ai) noexcept;

/// The fields that show whether the seat's controller holds the member.
struct LeaseState
{
    const void* memberEffective  = nullptr;  // member +0x580
    const void* memberRetainedAi = nullptr;  // member +0x588
    const void* aiPawn           = nullptr;  // (member +0x588) +0x258
    const void* seatPawn         = nullptr;  // the seat's controller +0x258
    const void* seatLeader       = nullptr;  // the seat's controller +0x5A0
};

/// The seat's controller drives the member, and names it as both its pawn and its leader.
bool Acquired(const LeaseState& state, const void* member, const void* seatController) noexcept;

/// The member is back with its own AI, and the seat's controller names nothing.
bool Released(const LeaseState& state, const void* member) noexcept;

// ---- a synergy partner the game has not given back yet (RE-p2-synergy-skills.md S-SYNTAKE §3, §5) ----
//
// The game hands a synergy partner to its AI for the approach and gives it back only in
// LinkMember_End (the sync start, the record end, or a mode-2 partner's own command end). No leader
// switch path of the game tests the link, so a deliberate switch onto an approaching partner stalls
// the approach; a seat's take is automatic, not a press, so it waits for the game's give-back as the
// game's own command menu treats an awaited partner as busy.

inline constexpr std::uint32_t kLinkRecordSize = 264;

/// One record of the battle system's link manager: the bytes the gate reads (the same offsets in both
/// builds).
struct LinkRecordView
{
    std::uint8_t types[3]{};      // +0x29+idx the members' player types
    std::uint8_t partner = 0xFF;  // +0x39 the partner's index
    std::uint8_t state[3]{};      // +0x56+idx 1 deferred start, 2 BT flag, 3 AI requested, 4 executing; 0 after
                                  // LinkMember_End
    std::uint8_t started[3]{};    // +0x100+idx
    std::uint8_t ended[3]{};      // +0x103+idx
    std::uint8_t mode = 0;        // the ability's +0x8F ExecuteConditionType0 (1-4; 0 without an ability)
    std::uint8_t kind = 0xFF;     // the ability's +0x8B CombinationCategory (1 ATB synergy ability, 0 synergy skill)
    std::uint8_t noLink = 0;      // +0x3A the halves run without the link flag (logged only; S-SYNSTUCK2 §4)
    std::uint8_t window = 0;      // +0x50 the combination window: the initiator's animation notify holds it open while
                                  // the game may ask for a mode-2 partner (S-SYNCOUNTER Q2)
    std::uint32_t actions[3]{};   // +0x2C+4*idx the members' command serials (logged only; not skill ids)
};

/// `playerType` is this record's partner and the game has not given it back yet: the record is live
/// (LinkRecord_AllMembersEnded is false) and the partner is on its approach (state 1-3), or a mode-2
/// partner plays its own half (started, not ended).
bool LinkPartnerBusy(const LinkRecordView& record, std::uint8_t playerType) noexcept;

/// `playerType` is this record's partner and has not ended there (the record to read for LinkPartnerBusy).
bool IsPartnerOf(const LinkRecordView& record, std::uint8_t playerType) noexcept;

// ---- the member in the seat's slot: whether the seat takes it now, or waits on it ----

/// Why the seat has no member to take now.
enum class SlotWait
{
    None,         // `member` is the one to take
    NoSlot,       // the party has no member but player 1's (a solo story party): nothing to wait on here
    NotEligible,  // the slot's member is not free by the switch's own member test (yet)
    AiNotReady,   // the slot's member is held by a story script, or not following the party
    LinkPartner,  // the slot's member is a synergy partner the game has not given back yet (`link`)
    Unread,       // the party reads faulted (objects torn down under them, as in a reload)
};

/// The slot's member is taken only when free by the switch's own member test, with its AI in a state the game
/// hands players, and not while it is a synergy partner the game has not given back: None, or the first reason
/// in that order.
SlotWait SlotWaitOf(Eligibility verdict, AiReadiness readiness, bool linkBusy) noexcept;

// ---- the take and the swap, awaited over the following ticks ----

enum class TakeOutcome
{
    Wait,
    Took,      // the seat's controller holds the member
    TimedOut,  // not within `takeTicks`: given back; the seat stays in on its own controller
};

/// The take's state this tick: acquired wins at any tick, the timeout only after it.
TakeOutcome TakeOutcomeOf(bool acquired, std::uint32_t ticks, std::uint32_t takeTicks) noexcept;

enum class SwapOutcome
{
    Wait,
    Released,    // back with its AI: choose again
    PlayerOnes,  // the game made it player 1's leader and the seat's controller holds nothing: choose again
    TimedOut,    // not back within `takeTicks`: the seat stays without a character
};

/// The give-back's state this tick: released first; then player 1's with the seat's controller empty; then the timeout.
SwapOutcome SwapOutcomeOf(bool released, const LeaseState& lease, const void* playerOneController, std::uint32_t ticks,
                          std::uint32_t takeTicks) noexcept;

}  // namespace lss::session
