// Whose state the command menu's gates ask about while a player's character is down (battle/downed_commands_rules.h).

#include "battle/downed_commands_rules.h"

#include "expect.h"

#include <cstdio>

namespace {

using namespace lss::battle::downed_commands_rules;

void TestCurrentMember()
{
    std::puts("current member: the palette's target, else the ally, else the menu's own member");
    EXPECT(CurrentMember(0, -1, -1) == 0);
    EXPECT(CurrentMember(0, 2, -1) == 2);
    EXPECT(CurrentMember(0, 2, 1) == 1);
    EXPECT(CurrentMember(0, -1, 2) == 2);
}

void TestAskAbout()
{
    std::puts("ask about: an ally the downed player commands, in co-op; never the menu's own member, another player's, "
              "or one out of the menu");
    EXPECT(AskAbout(true, 0, 2, 3, false) == 2);    // the AI ally through the shoulder buttons or the palette
    EXPECT(AskAbout(true, 0, 0, 3, false) == -1);   // the downed character's own menu: the game's answer (shut)
    EXPECT(AskAbout(false, 0, 2, 3, false) == -1);  // single player: the game's answer
    EXPECT(AskAbout(true, 0, 1, 3, true) == -1);    // another player's member: refused by the command rules
    EXPECT(AskAbout(true, 0, 3, 3, false) == -1);   // past the menu's members
    EXPECT(AskAbout(true, 0, -1, 3, false) == -1);
    EXPECT(AskAbout(true, -1, 2, 3, false) == -1);  // no menu member
    EXPECT(AskAbout(true, 0, 2, 3, false) == 2 && AskAbout(true, 1, 2, 3, false) == 2);  // the last member is in range
    // The palette held on the downed character itself: its own commands, the game's answer.
    EXPECT(AskAbout(true, 0, CurrentMember(0, 2, 0), 3, false) == -1);
}

}  // namespace

int main()
{
    TestCurrentMember();
    TestAskAbout();
    return lss::test::Report();
}
