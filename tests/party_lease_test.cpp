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

void TestSecondSlot()
{
    std::puts("slot: the 2nd slot is the row after the field leader, wrapping");
    const std::uint8_t squad[] = {0, 5, 3};  // Cloud, Yuffie, Aerith (the chapter 7 log)
    EXPECT(LeaderOrdinal(squad, 3, 0) == 0);
    EXPECT(OrdinalAfter(LeaderOrdinal(squad, 3, 0), 1, 3) == 1);
    EXPECT(LeaderOrdinal(squad, 3, 5) == 1);  // a story leader: the anchor moves with it
    EXPECT(OrdinalAfter(LeaderOrdinal(squad, 3, 5), 1, 3) == 2);
    EXPECT(OrdinalAfter(LeaderOrdinal(squad, 3, 3), 1, 3) == 0);  // wraps
    EXPECT(LeaderOrdinal(squad, 3, 7) == 0);                      // the leader not listed: the top row
    EXPECT(LeaderOrdinal(nullptr, 0, 0) == 0);
}

void TestAiReadiness()
{
    std::puts("ai: only the base brain's FOLLOW (or BATTLE) is taken; LEAD is given back");
    EXPECT(AssessAi({true, 0, kAiStateFollow}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 0, kAiStateBattle}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 2, kAiStateBattle}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 17, 1}) == AiReadiness::ScriptBrain);              // a scene's DO_NOTHING
    EXPECT(AssessAi({true, 3, kAiStateFollow}) == AiReadiness::ScriptBrain);  // a script's FollowActor
    EXPECT(AssessAi({true, 0, kAiStateLead}) == AiReadiness::NotFollow);
    EXPECT(AssessAi({}) == AiReadiness::Unread);
    EXPECT(AiTakesBack({true, 4, kAiStateLead}));
    EXPECT(AiTakesBack({true, 0, kAiStateLead}));
    EXPECT(!AiTakesBack({true, 10, 1}));  // a scene's park is not acted on
    EXPECT(!AiTakesBack({true, 0, kAiStateFollow}));
    EXPECT(!AiTakesBack({}));
    for (const auto readiness :
         {AiReadiness::Ready, AiReadiness::Unread, AiReadiness::ScriptBrain, AiReadiness::NotFollow})
    {
        EXPECT(Describe(readiness) != nullptr);
    }
}

void TestLinkPartnerBusy()
{
    std::puts("link: a synergy partner the game has not given back is busy; its initiator and others are not");
    LinkRecordView record;  // initiator Cloud (0) at index 0, partner Yuffie (5) at index 1, on its approach
    record.types[0]   = 0;
    record.types[1]   = 5;
    record.partner    = 1;
    record.started[0] = 1;
    record.mode       = 1;
    for (const std::uint8_t state : {1, 2, 3})
    {
        record.state[1] = state;
        EXPECT(LinkPartnerBusy(record, 5));
    }
    EXPECT(!LinkPartnerBusy(record, 0));  // the initiator: its half is driven by its component
    EXPECT(!LinkPartnerBusy(record, 3));  // not in this record

    auto given = record;
    given.state[1] = 0;  // LinkMember_End gave it back
    EXPECT(!LinkPartnerBusy(given, 5));
    given.state[1] = 4;  // executing: re-possessed at the sync start
    EXPECT(!LinkPartnerBusy(given, 5));

    auto ended = record;
    ended.ended[0] = 1;  // every started member ended: the record is going away
    EXPECT(!LinkPartnerBusy(ended, 5));
    auto none = record;
    none.started[0] = 0;
    EXPECT(!LinkPartnerBusy(none, 5));

    auto ownHalf       = given;  // mode 2: the partner plays its own half, still with its AI
    ownHalf.mode       = 2;
    ownHalf.started[1] = 1;
    EXPECT(LinkPartnerBusy(ownHalf, 5));
    ownHalf.ended[1] = 1;
    EXPECT(!LinkPartnerBusy(ownHalf, 5));  // its half ended; the initiator's alone keeps the record live

    auto modeTwoApproach = record;  // mode 2 on its approach: busy by its state alone
    modeTwoApproach.mode = 2;
    EXPECT(LinkPartnerBusy(modeTwoApproach, 5));
    auto noAbility = record;  // no ability read: the approach state still decides
    noAbility.mode = 0;
    EXPECT(LinkPartnerBusy(noAbility, 5));

    auto partnerLive       = given;  // live only through the partner (initiator ended), mode 1, executing
    partnerLive.ended[0]   = 1;
    partnerLive.started[1] = 1;
    EXPECT(!LinkPartnerBusy(partnerLive, 5));

    auto bad    = record;
    bad.partner = 3;  // an index out of the record
    EXPECT(!LinkPartnerBusy(bad, 5));
    bad.partner = 0xFF;  // the view's default: no record read
    EXPECT(!LinkPartnerBusy(bad, 5));
    EXPECT(!LinkPartnerBusy(LinkRecordView{}, 0));
}

int main()
{
    TestSecondSlot();
    TestLinkPartnerBusy();
    TestAiReadiness();
    TestAiOwnedMemberIsEligible();
    TestEachRuleRefuses();
    TestPlayerOnesCharacterWinsOverEverything();
    TestEveryVerdictHasText();
    TestAcquired();
    TestReleased();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
