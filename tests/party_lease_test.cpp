// Host-side tests for the party-member lease rules (session/party_lease.h), run without the game.
//
// The objects are stand-in addresses. An AI-owned member is the shape the game has for a party
// member nobody plays: every lease field names its AI controller, and the AI names it back.

#include "session/party_lease.h"

#include "facts/party.h"
#include "expect.h"

#include <cstdio>
#include <initializer_list>

namespace {

using namespace lss::session;
using lss::facts::kAiStateBattle;
using lss::facts::kAiStateFollow;
using lss::facts::kAiStateLead;
using lss::facts::kAiStateAnimation;
using lss::facts::kAiStateDoNothing;
using lss::facts::kAiStateMoveTo;
using lss::facts::kAiStateStay;

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
                               Eligibility::PlayerDriven, Eligibility::AiInvalid, Eligibility::BattleUnavailable})
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

    auto partial     = state;
    partial.seatPawn = nullptr;  // the leader is published but Unreal possession has not happened
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

    partial            = state;
    partial.seatLeader = &g_member;
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

void TestSeatSlots()
{
    std::puts("slot: player 2 plays the 2nd slot, player 3 the 3rd; the field leader's own row is never a slot");
    const std::uint8_t squad[] = {0, 5, 3};  // Cloud (the field leader), Yuffie, Aerith
    const auto none = [](std::int32_t) { return false; };
    EXPECT(SeatSlotOrdinal(0, 3, 1, none) == 1);   // player 2: Yuffie
    EXPECT(SeatSlotOrdinal(0, 3, 2, none) == 2);   // player 3: Aerith
    EXPECT(SeatSlotOrdinal(0, 3, 3, none) == -1);  // no 4th slot in three
    // A story leader (Yuffie) at ordinal 1: the rows run from her and wrap; her own row is never a slot.
    const std::int32_t anchor = LeaderOrdinal(squad, 3, 5);
    EXPECT(SeatSlotOrdinal(anchor, 3, 1, none) == 2);   // Aerith
    EXPECT(SeatSlotOrdinal(anchor, 3, 2, none) == 0);   // Cloud
    EXPECT(SeatSlotOrdinal(anchor, 3, 3, none) == -1);  // not Yuffie herself
    // An empty row (a type of none) is passed over; player 1's character is not (RowOrFieldLeader decides for it).
    const std::uint8_t gapped[] = {0, 0x10, 3, 2};
    const auto empty = [&](std::int32_t ordinal) { return gapped[ordinal] >= kNoStandIn; };
    EXPECT(SeatSlotOrdinal(0, 4, 1, empty) == 2);
    EXPECT(SeatSlotOrdinal(0, 4, 2, empty) == 3);
    // Two members: player 3 has no slot; the 1-player party: neither.
    EXPECT(SeatSlotOrdinal(0, 2, 1, none) == 1 && SeatSlotOrdinal(0, 2, 2, none) == -1);
    EXPECT(SeatSlotOrdinal(0, 1, 1, none) == -1);
    EXPECT(SeatSlotOrdinal(0, 0, 1, none) == -1 && SeatSlotOrdinal(0, 3, 0, none) == -1);
}

void TestRowOrFieldLeader()
{
    std::puts("slot: a row player 1 took gives the field leader's row instead, while the field leader is listed");
    EXPECT(RowOrFieldLeader(1, false, 0, true) == 1);   // the seat's own row, nobody's
    EXPECT(RowOrFieldLeader(1, true, 0, true) == 0);    // player 1 on it (the battle leader option, a switch): Cloud's row
    EXPECT(RowOrFieldLeader(2, true, 1, true) == 1);    // a story leader's row as the anchor
    EXPECT(RowOrFieldLeader(1, true, 0, false) == 1);   // the field leader not listed: the row stands (refused as player 1's)
    EXPECT(RowOrFieldLeader(2, false, 0, false) == 2);
}

void TestStandIn()
{
    std::puts("stand-in: the type player 1 came from, unless the change goes home, is unknown or is the seat's own");
    EXPECT(StandInFor(false, 0, 5) == 0);              // Cloud's player moved onto Yuffie: her player plays Cloud
    EXPECT(StandInFor(true, 5, 0) == kNoStandIn);      // the game's leader goes home to Cloud: no mark
    EXPECT(StandInFor(false, kNoStandIn, 5) == kNoStandIn);
    EXPECT(StandInFor(false, 0xFF, 5) == kNoStandIn);  // unknown
    EXPECT(StandInFor(false, 5, 5) == kNoStandIn);     // its own type
    const std::uint8_t squad[] = {0, 5, 3};
    EXPECT(OrdinalOfType(squad, 3, 0) == 0 && OrdinalOfType(squad, 3, 3) == 2);
    EXPECT(OrdinalOfType(squad, 3, 2) == -1);           // not deployed
    EXPECT(OrdinalOfType(squad, 3, kNoStandIn) == -1);  // no mark
    EXPECT(OrdinalOfType(nullptr, 0, 0) == -1);
}

void TestAiReadiness()
{
    std::puts("ai: the base brain's FOLLOW, BATTLE or a level's stay or stand is taken; a script's walk, lead or animation waits");
    EXPECT(AssessAi({true, 0, kAiStateFollow}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 0, kAiStateBattle}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 2, kAiStateBattle}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 2, kAiStateStay}) == AiReadiness::Ready);              // a level's stay (Sephiroth in Nibelheim)
    EXPECT(AssessAi({true, 8, kAiStateStay}) == AiReadiness::Ready);              // a setting group's stay between steps
    EXPECT(AssessAi({true, 2, kAiStateDoNothing}) == AiReadiness::Ready);         // the level's Stable setting: it stands
    EXPECT(AssessAi({true, 8, kAiStateDoNothing}) == AiReadiness::Ready);
    EXPECT(AssessAi({true, 8, kAiStateFollow}) == AiReadiness::Ready);            // a setting group's follow
    EXPECT(AssessAi({true, 2, kAiStateMoveTo}) == AiReadiness::ScriptBrain);      // a level's run to a mark
    EXPECT(AssessAi({true, 8, kAiStateMoveTo}) == AiReadiness::ScriptBrain);
    EXPECT(AssessAi({true, 2, kAiStateLead}) == AiReadiness::ScriptBrain);        // a level's lead
    EXPECT(AssessAi({true, 2, kAiStateAnimation}) == AiReadiness::ScriptBrain);   // a level's animation
    EXPECT(AssessAi({true, 16, kAiStateStay}) == AiReadiness::ScriptBrain);       // an event command's brain
    EXPECT(AssessAi({true, 2, 8}) == AiReadiness::ScriptBrain);                   // a level state not read for the take (WANDERING)
    EXPECT(AssessAi({true, 17, kAiStateDoNothing}) == AiReadiness::ScriptBrain);  // a scene's park
    EXPECT(AssessAi({true, 3, kAiStateFollow}) == AiReadiness::ScriptBrain);      // a script's FollowActor
    EXPECT(AssessAi({true, 0, kAiStateLead}) == AiReadiness::NotFollow);
    EXPECT(AssessAi({true, 0, kAiStateStay}) == AiReadiness::NotFollow);          // the base brain takes only its follow
    EXPECT(AssessAi({}) == AiReadiness::Unread);
    using lss::session::field_hold::Hold;
    EXPECT(HoldOf({true, 2, kAiStateLead}) == Hold::Arrival);  // the level's lead (Corel, live)
    EXPECT(HoldOf({true, 2, lss::facts::kAiStateMoveTo}) == Hold::Arrival);
    EXPECT(HoldOf({true, 16, lss::facts::kAiStateMoveTo}) == Hold::Script);  // an event's walk to a mark
    EXPECT(HoldOf({true, 2, lss::facts::kAiStateAnimation}) == Hold::Animation);
    EXPECT(HoldOf({true, 17, 1}) == Hold::Free);  // a scene's park is not acted on
    EXPECT(HoldOf({true, 0, kAiStateFollow}) == Hold::Free);
    EXPECT(HoldOf({true, 0, kAiStateLead}) == Hold::Free);  // the base brain is the follow's: the level writes on 2 or 8
    EXPECT(HoldOf({}) == Hold::Free);
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

void TestSlotWait()
{
    std::puts("slot wait: not eligible beats an unready AI, which beats a busy synergy partner, which beats a field "
              "action; else the member is taken");
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::Ready, false, false) == SlotWait::None);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::Ready, true, false) == SlotWait::LinkPartner);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::ScriptBrain, true, false) == SlotWait::AiNotReady);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::NotFollow, false, false) == SlotWait::AiNotReady);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::Unread, false, false) == SlotWait::AiNotReady);
    EXPECT(SlotWaitOf(Eligibility::Requested, AiReadiness::ScriptBrain, true, false) == SlotWait::NotEligible);
    EXPECT(SlotWaitOf(Eligibility::PlayerOnes, AiReadiness::Ready, false, false) == SlotWait::NotEligible);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::Ready, false, true) == SlotWait::FieldAction);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::Ready, true, true) == SlotWait::LinkPartner);
    EXPECT(SlotWaitOf(Eligibility::Eligible, AiReadiness::NotFollow, false, true) == SlotWait::AiNotReady);
    EXPECT(SlotWaitOf(Eligibility::PlayerOnes, AiReadiness::Ready, false, true) == SlotWait::NotEligible);
}

void TestIsPartnerOf()
{
    std::puts("link: the record whose partner is the player type and has not ended there");
    LinkRecordView record;
    record.types[0] = 0;
    record.types[1] = 5;
    record.partner  = 1;
    EXPECT(IsPartnerOf(record, 5));
    EXPECT(!IsPartnerOf(record, 0));  // the initiator
    EXPECT(!IsPartnerOf(record, 3));
    record.ended[1] = 1;
    EXPECT(!IsPartnerOf(record, 5));  // given back
    record.ended[1] = 0;
    record.partner  = 3;  // out of the record
    EXPECT(!IsPartnerOf(record, 5));
    EXPECT(!IsPartnerOf(LinkRecordView{}, 0));
}

void TestTakeAndSwapOutcomes()
{
    std::puts("take: acquired at any tick wins, the timeout only after it; swap: released, then player 1's with "
              "player 2's controller empty, then the timeout");
    EXPECT(TakeOutcomeOf(true, 0, 120) == TakeOutcome::Took);
    EXPECT(TakeOutcomeOf(true, 500, 120) == TakeOutcome::Took);
    EXPECT(TakeOutcomeOf(false, 0, 120) == TakeOutcome::Wait);
    EXPECT(TakeOutcomeOf(false, 119, 120) == TakeOutcome::Wait);
    EXPECT(TakeOutcomeOf(false, 120, 120) == TakeOutcome::TimedOut);

    int        playerOne;
    LeaseState lease;
    EXPECT(SwapOutcomeOf(true, lease, &playerOne, 0, 120) == SwapOutcome::Released);
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 0, 120) == SwapOutcome::Wait);
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 119, 120) == SwapOutcome::Wait);
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 120, 120) == SwapOutcome::TimedOut);

    lease.memberEffective = &playerOne;  // the game made it player 1's leader
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 0, 120) == SwapOutcome::PlayerOnes);
    EXPECT(SwapOutcomeOf(true, lease, &playerOne, 0, 120) == SwapOutcome::Released);  // released first
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 500, 120) == SwapOutcome::PlayerOnes);  // before the timeout
    lease.seatPawn = &g_member;  // the seat's controller still holds it: not yet
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 0, 120) == SwapOutcome::Wait);
    lease.seatPawn   = nullptr;
    lease.seatLeader = &g_member;
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 0, 120) == SwapOutcome::Wait);
    lease.seatLeader      = nullptr;
    lease.memberEffective = &g_other;  // another controller's: wait for the release
    EXPECT(SwapOutcomeOf(false, lease, &playerOne, 0, 120) == SwapOutcome::Wait);
}

int main()
{
    TestSecondSlot();
    TestSeatSlots();
    TestRowOrFieldLeader();
    TestStandIn();
    TestLinkPartnerBusy();
    TestAiReadiness();
    TestAiOwnedMemberIsEligible();
    TestEachRuleRefuses();
    TestPlayerOnesCharacterWinsOverEverything();
    TestEveryVerdictHasText();
    TestAcquired();
    TestReleased();
    TestSlotWait();
    TestIsPartnerOf();
    TestTakeAndSwapOutcomes();

    return lss::test::Report();
}
