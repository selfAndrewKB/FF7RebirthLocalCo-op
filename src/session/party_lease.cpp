#include "session/party_lease.h"

#include "facts/party.h"

#include <iterator>

namespace lss::session {

using facts::kAiStateBattle;
using facts::kAiStateDoNothing;
using facts::kAiStateFollow;
using facts::kAiStateStay;

static_assert(kNoStandIn == facts::kNoPlayerType, "no stand-in is the roster's none");

Eligibility Assess(const MemberSnapshot& member) noexcept
{
    if (member.character == nullptr)
    {
        return Eligibility::NoCharacter;
    }
    if (!member.isEndCharacter)
    {
        return Eligibility::NotEndCharacter;
    }
    if (member.isPlayerOnes)
    {
        return Eligibility::PlayerOnes;
    }
    if (member.requested != nullptr)
    {
        return Eligibility::Requested;
    }
    const void* ai = member.retainedAi;
    if (ai == nullptr || member.effective != ai || member.controller != ai || member.aiPawn != member.character)
    {
        return Eligibility::NotAiOwned;
    }
    if (member.playerControlled != 0)
    {
        return Eligibility::PlayerDriven;
    }
    if (!member.aiControllerValid)
    {
        return Eligibility::AiInvalid;
    }
    if (!member.battleReady)
    {
        return Eligibility::BattleUnavailable;
    }
    return Eligibility::Eligible;
}

const wchar_t* Describe(Eligibility eligibility) noexcept
{
    switch (eligibility)
    {
        case Eligibility::Eligible: return L"eligible";
        case Eligibility::NoCharacter: return L"no character loaded";
        case Eligibility::NotEndCharacter: return L"not an End character";
        case Eligibility::PlayerOnes: return L"player 1's character";
        case Eligibility::Requested: return L"already requested by a controller";
        case Eligibility::NotAiOwned: return L"not owned by its AI";
        case Eligibility::PlayerDriven: return L"marked player-controlled";
        case Eligibility::AiInvalid: return L"its AI controller is not valid";
        case Eligibility::BattleUnavailable: return L"unavailable to the battle switch";
    }
    return L"unknown";
}

std::int32_t LeaderOrdinal(const std::uint8_t* types, std::int32_t count, std::uint8_t fieldLeader) noexcept
{
    for (std::int32_t ordinal = 0; types != nullptr && ordinal < count; ++ordinal)
    {
        if (types[ordinal] == fieldLeader)
        {
            return ordinal;
        }
    }
    return 0;
}

AiReadiness AssessAi(const AiSnapshot& ai) noexcept
{
    if (!ai.read)
    {
        return AiReadiness::Unread;
    }
    if (ai.state == kAiStateBattle)
    {
        return AiReadiness::Ready;  // the game's own battle switch hands members over in it
    }
    if (ai.brainIndex != 0)
    {
        // A level setting that only parks the member (a stay, a stand, a group's follow) is the player's once held
        // (field_hold::Of), so a join takes it the same way: the follow's catch-up speed-up is gone at its exit and a stay
        // leaves nothing on the pawn (research/RE-p2-member-handover.md, joining in a level STAY). A level's walk, lead or
        // animation, and every other brain (an event's, a speaker's, a scene's park) wait.
        const bool parked = ai.state == kAiStateStay || ai.state == kAiStateDoNothing || ai.state == kAiStateFollow;
        return field_hold::LevelBrain(ai.brainIndex) && parked ? AiReadiness::Ready : AiReadiness::ScriptBrain;
    }
    return ai.state == kAiStateFollow ? AiReadiness::Ready : AiReadiness::NotFollow;
}

const wchar_t* Describe(AiReadiness readiness) noexcept
{
    switch (readiness)
    {
        case AiReadiness::Ready: return L"ready";
        case AiReadiness::Unread: return L"its AI could not be read";
        case AiReadiness::ScriptBrain: return L"another AI brain holds it (a story script, or a vehicle seat)";
        case AiReadiness::NotFollow: return L"its AI is not following the party";
    }
    return L"unknown";
}

field_hold::Hold HoldOf(const AiSnapshot& ai) noexcept
{
    return ai.read ? field_hold::Of(ai.brainIndex, ai.state) : field_hold::Hold::Free;
}

bool Acquired(const LeaseState& state, const void* member, const void* seatController) noexcept
{
    return member != nullptr && seatController != nullptr && state.memberEffective == seatController &&
           state.seatPawn == member && state.seatLeader == member;
}

bool Released(const LeaseState& state, const void* member) noexcept
{
    const void* ai = state.memberRetainedAi;
    return member != nullptr && ai != nullptr && state.memberEffective == ai && state.aiPawn == member &&
           state.seatPawn == nullptr && state.seatLeader == nullptr;
}

bool LinkPartnerBusy(const LinkRecordView& record, std::uint8_t playerType) noexcept
{
    const std::uint8_t partner = record.partner;
    if (partner >= std::size(record.types) || record.types[partner] != playerType)
    {
        return false;
    }
    bool live = false;  // !LinkRecord_AllMembersEnded: some member started and has not ended
    for (std::size_t index = 0; index < std::size(record.started); ++index)
    {
        live = live || (record.started[index] != 0 && record.ended[index] == 0);
    }
    if (!live)
    {
        return false;
    }
    const std::uint8_t state       = record.state[partner];
    const bool         approaching = state >= 1 && state <= 3;
    const bool         ownHalf     = record.mode == 2 && record.started[partner] != 0 && record.ended[partner] == 0;
    return approaching || ownHalf;
}

bool IsPartnerOf(const LinkRecordView& record, std::uint8_t playerType) noexcept
{
    return record.partner < std::size(record.types) && record.types[record.partner] == playerType &&
           record.ended[record.partner] == 0;
}

SlotWait SlotWaitOf(Eligibility verdict, AiReadiness readiness, bool linkBusy, bool inFieldAction) noexcept
{
    if (verdict != Eligibility::Eligible)
    {
        return SlotWait::NotEligible;
    }
    if (readiness != AiReadiness::Ready)
    {
        return SlotWait::AiNotReady;
    }
    if (linkBusy)
    {
        // A synergy partner on its approach (its AI's, in BATTLE): taken only once the game gives it back.
        return SlotWait::LinkPartner;
    }
    if (inFieldAction)
    {
        // A climb its AI began, taken mid-way, would end at the ladder's foot and leave the walk it was part of short.
        return SlotWait::FieldAction;
    }
    return SlotWait::None;
}

bool OnGameAi(SlotWait wait) noexcept
{
    return wait == SlotWait::AiNotReady || wait == SlotWait::LinkPartner || wait == SlotWait::FieldAction;
}

TakeOutcome TakeOutcomeOf(bool acquired, std::uint32_t ticks, std::uint32_t takeTicks) noexcept
{
    if (acquired)
    {
        return TakeOutcome::Took;
    }
    return ticks < takeTicks ? TakeOutcome::Wait : TakeOutcome::TimedOut;
}

SwapOutcome SwapOutcomeOf(bool released, const LeaseState& lease, const void* playerOneController, std::uint32_t ticks,
                          std::uint32_t takeTicks) noexcept
{
    if (released)
    {
        return SwapOutcome::Released;
    }
    // Given back as the game made it player 1's leader: it never returns to its AI.
    if (lease.memberEffective == playerOneController && lease.seatPawn == nullptr && lease.seatLeader == nullptr)
    {
        return SwapOutcome::PlayerOnes;
    }
    return ticks < takeTicks ? SwapOutcome::Wait : SwapOutcome::TimedOut;
}

}  // namespace lss::session
