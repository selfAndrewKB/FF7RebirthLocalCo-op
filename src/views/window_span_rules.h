#pragma once

// The window span's pure rules, tested on the host (tests/window_span_rules_test.cpp).
//
// PresetWait: the settings file asks for the three-player monitor mode (multi-monitor = on) before the engine has
// sized its window to the saved resolution, so the one try waits until the game window has read the same rect for a
// run of ticks; a window that never settles (or cannot be reached) is given up after a longer run, logged, and the key
// or the console tries later.

#include "views/monitor_row.h"

#include <cstdint>
#include <optional>

namespace lss::views::window_span {

struct PresetWait
{
    enum class Step
    {
        Wait,    // not yet
        Try,     // the window has been steady long enough: try the mode now, once
        GiveUp,  // too long without a steady window: stop waiting
    };

    std::uint32_t    ticks  = 0;  // ticks waited
    std::uint32_t    steady = 0;  // ticks the rect has been the same
    monitor_row::Rect rect;       // the rect last seen

    /// One tick with the window's rect as read now (nothing when the window cannot be reached). Try comes once the
    /// rect has been read the same `steadyTicks` ticks in a row; GiveUp once `giveUpTicks` ticks passed without it.
    Step Next(const std::optional<monitor_row::Rect>& seen, std::uint32_t steadyTicks, std::uint32_t giveUpTicks) noexcept
    {
        ++ticks;
        if (seen)
        {
            if (*seen == rect)
            {
                ++steady;
            }
            else
            {
                rect   = *seen;
                steady = 0;
            }
            if (steady >= steadyTicks)
            {
                return Step::Try;
            }
        }
        else
        {
            steady = 0;
        }
        return ticks > giveUpTicks ? Step::GiveUp : Step::Wait;
    }
};

}  // namespace lss::views::window_span
