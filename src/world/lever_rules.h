#pragma once

// Which switches (levers, wall switches, lift panels: the game's switch-access actors; world/levers.h) players 2 and 3
// may use, and two small decisions of the module, as pure functions tested on the host (tests/lever_rules_test.cpp).
//
// The list holds every switch whose press, followed through the level's own state triggers, starts no scene and
// names no character (research/RE-further-player-levers.md: a lift, a gate, a rail route, a story flag). A switch
// whose press starts a cutscene or a planned event, moves or turns a named character, or is pulled by the script
// itself stays player 1's, as does any switch not listed. The list is the output of
// research/.asset-scripts/levers_trace.py --cpp over the game's field data; regenerate it there, never by hand.

#include <array>
#include <cstddef>
#include <string_view>

namespace lss::world::lever_rules {

inline constexpr std::array<std::wstring_view, 26> kSwitches{{
    L"obl2200_Gmk_LiftSwitch_DOWN",
    L"obl2200_Gmk_LiftSwitch_UP",
    L"obl2250_Lever_GateHeliGunnerKaiAreaEntrance",
    L"obl2250_Lever_GateHeliGunnerKaiAreaStairs",
    L"obl2250_Lever_RailwayRoute",
    L"obl2250_Switch_Elevator_InLower",
    L"obl2250_Switch_Elevator_InUpper",
    L"obl2250_Switch_Elevator_Lower",
    L"obl2250_Switch_Elevator_Upper",
    L"obl2250_Switch_LiftA_In",
    L"obl2250_Switch_LiftB_In",
    L"obl2350_wallswitch_ghostElevator1F",
    L"obl2350_wallswitch_ghostElevator3F",
    L"obl2500_LeverSkeltalMesh_Lift1",
    L"obl2500_Lever_Lift2",
    L"obl2500_Lever_Lift3",
    L"obl2500_Lever_Lift4",
    L"obl2660_Lever_AfterUnderPipe",
    L"obl2660_Lever_Gimmick2",
    L"obl2660_Switch_Elevator1_2_1",
    L"obl2660_Switch_Elevator1_2_2",
    L"obl2660_Switch_Elevator3_1",
    L"obl3110_Switch_Elv100",
    L"obl3110_Switch_Elv110",
    L"obl3110_Switch_Elv200",
    L"obl3110_Switch_Elv210",
}};

/// The game's names compare without case.
constexpr bool SameName(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const wchar_t x = a[i] >= L'A' && a[i] <= L'Z' ? static_cast<wchar_t>(a[i] - L'A' + L'a') : a[i];
        const wchar_t y = b[i] >= L'A' && b[i] <= L'Z' ? static_cast<wchar_t>(b[i] - L'A' + L'a') : b[i];
        if (x != y)
        {
            return false;
        }
    }
    return true;
}

/// A switch players 2 and 3 may use, by its placed actor's name or its point's key (both the level's row name).
constexpr bool Admitted(std::wstring_view name) noexcept
{
    for (const std::wstring_view row : kSwitches)
    {
        if (SameName(row, name))
        {
            return true;
        }
    }
    return false;
}

/// The switch's press latch is one per actor, and every holder's notify that sees it up would start the switch move
/// on its own character: a notify for `notified` (0 player 1, a seat; -1 no player's character) runs with the latch
/// down unless it is the presser's (`presser`, -1 when nobody's press is pending or it is unknown).
constexpr bool LatchDownFor(int presser, int notified) noexcept
{
    return presser >= 0 && notified != presser;
}

}  // namespace lss::world::lever_rules
