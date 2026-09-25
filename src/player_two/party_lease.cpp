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
