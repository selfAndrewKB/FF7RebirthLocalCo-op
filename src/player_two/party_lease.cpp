#include "player_two/party_lease.h"

namespace lss::player_two {

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
    if (member.leaderSlot)
    {
        return Eligibility::LeaderSlot;
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
        case Eligibility::LeaderSlot: return L"the field or default battle leader's slot (player 1's)";
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
        return AiReadiness::ScriptBrain;
    }
    return ai.state == kAiStateFollow ? AiReadiness::Ready : AiReadiness::NotFollow;
}

const wchar_t* Describe(AiReadiness readiness) noexcept
{
    switch (readiness)
    {
        case AiReadiness::Ready: return L"ready";
        case AiReadiness::Unread: return L"its AI could not be read";
        case AiReadiness::ScriptBrain: return L"a story script holds its AI";
        case AiReadiness::NotFollow: return L"its AI is not following the party";
    }
    return L"unknown";
}

bool AiTakesBack(const AiSnapshot& ai) noexcept
{
    return ai.read && ai.state == kAiStateLead;
}

bool Acquired(const LeaseState& state, const void* member, const void* playerTwoController) noexcept
{
    return member != nullptr && playerTwoController != nullptr && state.memberEffective == playerTwoController &&
           state.playerTwoPawn == member && state.playerTwoLeader == member;
}

bool Released(const LeaseState& state, const void* member) noexcept
{
    const void* ai = state.memberRetainedAi;
    return member != nullptr && ai != nullptr && state.memberEffective == ai && state.aiPawn == member &&
           state.playerTwoPawn == nullptr && state.playerTwoLeader == nullptr;
}

}  // namespace lss::player_two
