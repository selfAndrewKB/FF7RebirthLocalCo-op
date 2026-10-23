// Host-side tests for player_two::field_rules, run without the game.
//
// The shopkeeper name's suffix, the game's mapped pointers and row names, player 2's trigger route and verdict, and
// the input clock's seconds-before.

#include "player_two/world/field_rules.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using namespace lss::player_two::field_rules;

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
    EXPECT(TriggerRouteOf(true, false, false) == TriggerRoute::PlayerTwo);
    EXPECT(TriggerRouteOf(false, true, true) == TriggerRoute::PlayerOne);
    EXPECT(TriggerRouteOf(false, true, false) == TriggerRoute::Original);
    EXPECT(TriggerRouteOf(false, false, true) == TriggerRoute::Original);
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
