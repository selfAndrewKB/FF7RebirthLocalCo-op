#pragma once

// Whose state the command menu's two gates ask about while a player's own character is down (battle/downed_commands.h).
// Pure (host-tested in tests/downed_commands_rules_test.cpp).

namespace lss::battle::downed_commands_rules {

/// The member a menu push opens for, as the game's own push picks it: the shortcut palette's target, else the ally
/// commanded through the shoulder buttons, else the menu's own member (a negative ordinal is none).
constexpr int CurrentMember(int menuMember, int ally, int shortcutTarget) noexcept
{
    return shortcutTarget >= 0 ? shortcutTarget : ally >= 0 ? ally : menuMember;
}

/// The member whose death or petrify the gate tests instead of the menu's own: the current member, in co-op, when it is
/// another member of the menu (an ally the player commands, as a stopped or sleeping leader still does), not one the
/// command rules refuse; -1 to ask the game's own question. `count` is the menu's member count.
constexpr int AskAbout(bool coop, int menuMember, int current, int count, bool currentRefused) noexcept
{
    if (!coop || menuMember < 0 || current < 0 || current >= count || current == menuMember || currentRefused)
    {
        return -1;
    }
    return current;
}

}  // namespace lss::battle::downed_commands_rules
