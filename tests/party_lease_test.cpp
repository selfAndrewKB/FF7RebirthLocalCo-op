// Host-side tests for the party-member lease rules (player_two/party_lease.h), run without the game.
//
// The objects are stand-in addresses. An AI-owned member is the shape the game has for a party
// member nobody plays: every lease field names its AI controller, and the AI names it back.

#include "player_two/party_lease.h"

#include <cstdio>
#include <initializer_list>

namespace {

using namespace lss::player_two;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

int g_member;
int g_ai;
int g_playerTwo;
int g_other;

MemberSnapshot AiOwned()
{
    MemberSnapshot member;
    member.character         = &g_member;
    member.isEndCharacter    = true;
    member.effective         = &g_ai;
    member.retainedAi        = &g_ai;
    member.controller        = &g_ai;
    member.aiPawn            = &g_member;
    member.aiControllerValid = true;
    return member;
}

void TestAiOwnedMemberIsEligible()
{
    std::puts("eligibility: an AI-owned party member is eligible");
    EXPECT(Assess(AiOwned()) == Eligibility::Eligible);
}

void TestEachRuleRefuses()
{
    std::puts("eligibility: each rule refuses on its own");
    auto member = AiOwned();
    member.character = nullptr;
    EXPECT(Assess(member) == Eligibility::NoCharacter);

    member = AiOwned();
    member.isEndCharacter = false;
    EXPECT(Assess(member) == Eligibility::NotEndCharacter);

    member = AiOwned();
    member.isPlayerOnes = true;
    EXPECT(Assess(member) == Eligibility::PlayerOnes);

    member = AiOwned();
    member.requested = &g_other;
    EXPECT(Assess(member) == Eligibility::Requested);

    member = AiOwned();
    member.effective = &g_other;
    EXPECT(Assess(member) == Eligibility::NotAiOwned);

    member = AiOwned();
    member.controller = &g_other;
    EXPECT(Assess(member) == Eligibility::NotAiOwned);

    member = AiOwned();
    member.aiPawn = &g_other;
    EXPECT(Assess(member) == Eligibility::NotAiOwned);

    member = AiOwned();
    member.retainedAi = nullptr;
    member.effective  = nullptr;
    member.controller = nullptr;
    EXPECT(Assess(member) == Eligibility::NotAiOwned);  // all agreeing on "none" is not AI-owned

    member = AiOwned();
    member.playerControlled = 1;
    EXPECT(Assess(member) == Eligibility::PlayerDriven);

    member = AiOwned();
    member.aiControllerValid = false;
    EXPECT(Assess(member) == Eligibility::AiInvalid);

    member = AiOwned();
    member.leaderSlot = true;
    EXPECT(Assess(member) == Eligibility::LeaderSlot);

    member = AiOwned();
    member.battleReady = false;
    EXPECT(Assess(member) == Eligibility::BattleUnavailable);
}

void TestPlayerOnesCharacterWinsOverEverything()
{
    std::puts("eligibility: player 1's character is refused even when it looks AI-owned");
    auto member         = AiOwned();
    member.isPlayerOnes = true;
    EXPECT(Assess(member) == Eligibility::PlayerOnes);
}

void TestEveryVerdictHasText()
{
    std::puts("eligibility: every verdict describes itself");
    for (const auto verdict : {Eligibility::Eligible, Eligibility::NoCharacter, Eligibility::NotEndCharacter,
                               Eligibility::PlayerOnes, Eligibility::Requested, Eligibility::NotAiOwned,
                               Eligibility::PlayerDriven, Eligibility::AiInvalid, Eligibility::LeaderSlot,
                               Eligibility::BattleUnavailable})
    {
        const wchar_t* text = Describe(verdict);
        EXPECT(text != nullptr && text[0] != L'\0' && text != Describe(static_cast<Eligibility>(99)));
    }
}

void TestAcquired()
{
    std::puts("lease: acquired only when player 2 drives the member and names it as pawn and leader");
    LeaseState state{&g_playerTwo, &g_ai, nullptr, &g_member, &g_member};
    EXPECT(Acquired(state, &g_member, &g_playerTwo));

    auto partial          = state;
    partial.playerTwoPawn = nullptr;  // the leader is published but Unreal possession has not happened
    EXPECT(!Acquired(partial, &g_member, &g_playerTwo));

    partial                 = state;
    partial.memberEffective = &g_ai;  // the switch's gate refused: leader published, control not moved
    EXPECT(!Acquired(partial, &g_member, &g_playerTwo));

    EXPECT(!Acquired(state, nullptr, &g_playerTwo));
    EXPECT(!Acquired(state, &g_member, nullptr));
}

void TestReleased()
{
    std::puts("lease: released only when the AI drives the member again and player 2 names nothing");
    LeaseState state{&g_ai, &g_ai, &g_member, nullptr, nullptr};
    EXPECT(Released(state, &g_member));

    auto partial            = state;
    partial.memberEffective = &g_playerTwo;  // the release is still queued
    EXPECT(!Released(partial, &g_member));

    partial        = state;
    partial.aiPawn = nullptr;  // the AI has not re-possessed yet
    EXPECT(!Released(partial, &g_member));

    partial                 = state;
    partial.playerTwoLeader = &g_member;
    EXPECT(!Released(partial, &g_member));

    partial                  = state;
    partial.memberRetainedAi = nullptr;
    partial.memberEffective  = nullptr;
    EXPECT(!Released(partial, &g_member));
}

}  // namespace

int main()
{
    TestAiOwnedMemberIsEligible();
    TestEachRuleRefuses();
    TestPlayerOnesCharacterWinsOverEverything();
    TestEveryVerdictHasText();
    TestAcquired();
    TestReleased();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
