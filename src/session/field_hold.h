#pragma once

// When a script needs a player-held party member back, and when it does not (research/RE-held-member-scripts.md).
//
// A member's AI controller is a stack of brains: brain 0 the base (FOLLOW), brain 2 the level's settings (a stay, a
// run to a mark, a lead, an animation; brain 8 is the setting group's other one), brain 16 an event command's, 10 and
// 14 the speakers', 12 and 17 a scene's park. A request written on a held member enters its state at once and never
// ticks after (the AI controller has no pawn while a player holds the member), so:
//   - a stay, a stand or a scene's park fires nothing and stalls nothing: the player keeps the member (no scene takes
//     the character from its controller; that the cut drives the body through its cinema mode is the research's
//     reading);
//   - a run to a mark or a lead on the level's brains fires its arrival trigger, which a level step may wait on: the
//     level's own data decides (Arrival: a guide goes to its AI, staging stays the player's);
//   - a run or a lead on any other brain (an event command's walk to a mark before a bind) may hold the event's next
//     command: the member goes to its AI (Script);
//   - an animation's state ticks only under the AI: the member goes to its AI, the player's view kept on it
//     (Animation; how an animation ends, and whether a script waits on it, is not read).
// A member taken during a move resumes it from where it stands when given back (no fresh start).
//
// Pure (host-tested in tests/field_hold_test.cpp).

#include "facts/party.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lss::session::field_hold {

/// The brains (the controller's brain stack, the active one's index byte).
inline constexpr std::uint8_t kBrainBase       = 0;
inline constexpr std::uint8_t kBrainLevel      = 2;
inline constexpr std::uint8_t kBrainLevelGroup = 8;  // a setting group's other brain (the group pops one, requests on the other)

enum class Hold : std::uint8_t
{
    Free,       // the player's: the follow, a stay, a stand, a scene's park, a battle
    Arrival,    // a run to a mark or a lead on the level's brains: staging or a guide, the level's data decides
    Script,     // a run or a lead a script other than the level's wrote (an event command's): its AI walks it
    Animation,  // a scripted animation: its AI plays it, the player's view on it
};

/// A level setting's brain.
constexpr bool LevelBrain(std::uint8_t brain) noexcept
{
    return brain == kBrainLevel || brain == kBrainLevelGroup;
}

/// The states whose arrival fires a trigger a level step can wait on: a run to a mark and a lead. The FollowActor and
/// RouteMove settings (MoveSetting) parse into states whose arrival is not read: a fourth trigger-firing state class
/// exists, unnamed.
constexpr bool SignalsArrival(std::uint8_t state) noexcept
{
    return state == facts::kAiStateMoveTo || state == facts::kAiStateLead;
}

/// The hold of a request on a member's AI: a brain and its state's type (the session reads the active brain each few
/// ticks; a request written beneath a scene's park enters as a stacked state and is seen when the park pops).
constexpr Hold Of(std::uint8_t brain, std::uint8_t state) noexcept
{
    if (brain == kBrainBase)
    {
        return Hold::Free;
    }
    if (state == facts::kAiStateAnimation || state == facts::kAiStateSingleAnimation)
    {
        return Hold::Animation;
    }
    if (SignalsArrival(state))
    {
        return LevelBrain(brain) ? Hold::Arrival : Hold::Script;
    }
    return Hold::Free;
}

/// ASCII letters compared without case, as the game's setting parser compares a row's Status.
constexpr bool SameIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const wchar_t x = a[i] >= L'A' && a[i] <= L'Z' ? static_cast<wchar_t>(a[i] + (L'a' - L'A')) : a[i];
        const wchar_t y = b[i] >= L'A' && b[i] <= L'Z' ? static_cast<wchar_t>(b[i] + (L'a' - L'A')) : b[i];
        if (x != y)
        {
            return false;
        }
    }
    return true;
}

/// The Status of a setting group row: its group (the row's first string argument) names the states.
inline constexpr std::wstring_view kGroupStatus = L"AIStateGroup";

/// A setting group's states (the AI system's group map, in table order; RE-walk-gate.md): whether one of
/// them is a run or a lead whose arrival names a trigger (its string slot 2 is filled: a group's first state is no
/// guide, LeadDefault opens with a stay), and whether the group follows the party (its states are FOLLOW).
struct GroupState
{
    std::uint8_t state       = 0;   // EEndAIState
    std::int8_t  triggerSlot = -1;  // the setting row's string argument that fills the state's slot 2, -1: none
};

constexpr bool GroupMoves(const GroupState* states, std::size_t count) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (SignalsArrival(states[i].state) && states[i].triggerSlot >= 0)
        {
            return true;
        }
    }
    return false;
}

constexpr bool GroupFollows(const GroupState* states, std::size_t count) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (states[i].state != facts::kAiStateFollow)
        {
            return false;
        }
    }
    return count > 0;
}

/// A level setting that moves the member: a Status the parser turns into a moving state, or a group row whose group
/// moves it (`groupMoves`, GroupMoves; ignored otherwise).
constexpr bool MoveSetting(std::wstring_view status, bool groupMoves) noexcept
{
    return SameIgnoringCase(status, L"Lead") || SameIgnoringCase(status, L"MoveToTarget") ||
           SameIgnoringCase(status, L"FollowActor") || SameIgnoringCase(status, L"RouteMove") ||
           (SameIgnoringCase(status, kGroupStatus) && groupMoves);
}

/// The end of a member's scripted path: the Stable setting (the member stands again), a row with no Status (the
/// release: brains 2 and 8 popped, the follow back), or a group row whose group follows the party again
/// (`groupFollows`, GroupFollows). A move after it is a later beat's, not this path's.
constexpr bool PathEnd(std::wstring_view status, bool groupFollows) noexcept
{
    return status.empty() || SameIgnoringCase(status, L"Stable") ||
           (SameIgnoringCase(status, kGroupStatus) && groupFollows);
}

}  // namespace lss::session::field_hold
