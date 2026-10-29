// Host-side tests for world::field_rules, run without the game.
//
// The shopkeeper name's suffix, the game's mapped pointers and row names, player 2's trigger route and verdict, and
// the input clock's seconds-before.

#include "world/field_rules.h"

#include "expect.h"

#include <cstdio>
#include <initializer_list>
#include <string>

namespace {

using namespace lss::world::field_rules;

void TestNames()
{
    std::puts("names: the text after the first underscore, lower case; a mapped row name is ASCII");
    EXPECT(AfterPrefix(L"Npc2100_shopweapon01") == L"shopweapon01");
    EXPECT(AfterPrefix(L"oba2100_ShopWeapon01") == L"shopweapon01");
    EXPECT(AfterPrefix(L"NoUnderscore") == L"nounderscore");
    EXPECT(AfterPrefix(L"a_b_c") == L"b_c");
    EXPECT(AfterPrefix(L"") == L"");
    EXPECT(IsRowName(L"Row_Name 01"));
    EXPECT(IsRowName(L""));
    EXPECT(!IsRowName(L"Row\tName"));
    EXPECT(!IsRowName(L"Roé"));
}

void TestMappedPointer()
{
    std::puts("mapped: a pointer as is; an odd value is a signed offset from the field, halved; zero is null");
    alignas(16) std::byte buffer[64]{};
    const std::byte*      field = buffer + 32;
    EXPECT(MappedPointer(field, 0) == nullptr);
    EXPECT(MappedPointer(field, 0x1000) == reinterpret_cast<const std::byte*>(0x1000));
    EXPECT(MappedPointer(field, (16 << 1) | 1) == field + 16);
    EXPECT(MappedPointer(field, static_cast<std::uint64_t>((-32LL) << 1) | 1) == field - 32);
}

void TestTriggers()
{
    std::puts("triggers: player 2's own, player 1's inside a switch volume, else the game's; the verdict");
    // By seat: the owner's seat routes the event, and names the seat. For two players (player 2's trigger = seat 1)
    // every answer is the one before the seats (v0.91.0: player 2's own, then player 1's while watching, else the
    // game's).
    for (const bool two : {false, true})
    {
        for (const bool one : {false, true})
        {
            for (const bool watching : {false, true})
            {
                const TriggerRoute before = two ? TriggerRoute::PlayerTwo
                                            : one && watching ? TriggerRoute::PlayerOne
                                                              : TriggerRoute::Original;
                EXPECT(TriggerRouteOf(two ? 1 : one ? 0 : -1, watching).route == before);
            }
        }
    }
    EXPECT(TriggerRouteOf(1, false).route == TriggerRoute::PlayerTwo && TriggerRouteOf(1, false).seat == 1);
    EXPECT(TriggerRouteOf(2, true).route == TriggerRoute::PlayerTwo && TriggerRouteOf(2, true).seat == 2);
    EXPECT(TriggerRouteOf(0, true).route == TriggerRoute::PlayerOne && TriggerRouteOf(0, true).seat == -1);
    EXPECT(TriggerRouteOf(0, false).route == TriggerRoute::Original);
    EXPECT(TriggerRouteOf(-1, true).route == TriggerRoute::Original && TriggerRouteOf(-1, true).seat == -1);
    EXPECT(TriggerVerdictOf(-1, false, 1) == TriggerVerdict::NotAdmitted);
    EXPECT(TriggerVerdictOf(2, true, 0) == TriggerVerdict::PromptZipLine);
    EXPECT(TriggerVerdictOf(2, true, 1) == TriggerVerdict::Deliver);
    EXPECT(TriggerVerdictOf(2, false, 0) == TriggerVerdict::Deliver);
}

void TestSecondsBefore()
{
    std::puts("seconds before: adding the frame's dt the game's way lands exactly on the target");
    for (const double target : {1.0, 123.456789, 100000.015625, 0.3})
    {
        for (const float dt : {1.0f / 60.0f, 0.0166f, 0.033f, 1.0f / 120.0f})
        {
            const double before = SecondsBefore(target, dt, 4);
            EXPECT(static_cast<double>(dt) + before == target);
        }
    }
}

}  // namespace

int main()
{
    TestNames();
    TestMappedPointer();
    TestTriggers();
    TestSecondsBefore();

    return lss::test::Report();
}
