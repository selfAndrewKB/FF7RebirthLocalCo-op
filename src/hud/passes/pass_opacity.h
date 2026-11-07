#pragma once

// Widgets taken out of one overlay pass's image: the other players' prompt rows (hud/field/prompt_rows.h), point
// markers (hud/field/field_markers.h), search pins (hud/field/search_pins.h), target marks (hud/battle/target_views.h)
// and command menu (battle/command_menu_view.h) are made transparent through the game's own setter
// (players/widget_opacity.h) and given their own opacity back after the pass. Not Hidden: a UMG widget is ticked only
// while it is painted, and a seat's pass runs with DeltaTime 0, so a row hidden in player 1's pass would never
// advance its timers (seen in play). Each module keeps what it hid in a HiddenSet and decides whose each widget is;
// the participants' ShowOnly and Restore run from the one pass driver (hud/passes/root_overlay_render.cpp) through the
// two functions here, in one order. Game thread.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lss::hud {

/// The widgets one module made transparent for the pass, each with its own opacity to give back.
class HiddenSet
{
public:
    /// Room for `count` widgets (guarded: a failed reserve changes nothing).
    void Reserve(std::size_t count) noexcept;

    /// Makes `widget` transparent for the pass, remembering its own opacity. `weak` is the widget's FWeakObjectPtr
    /// value as stored by its owner (0: taken from the widget). False, nothing changed, for no widget, a widget not
    /// drawn anyway (opacity 0), a setter not installed or faulting, or no room to remember it.
    bool Hide(void* widget, std::uint64_t weak = 0) noexcept;

    /// After the pass: every widget hidden here gets its own opacity back, while it still shows the pass's 0 (an
    /// opacity the game set since is its own). Then the set is empty.
    void RestoreAll() noexcept;

    /// Empties the set without widget calls (for a removal that may run off the game thread).
    void Forget() noexcept;

    bool Empty() const noexcept { return hidden_.empty(); }

private:
    struct Hidden
    {
        std::uint64_t weak     = 0;     // the widget's FWeakObjectPtr
        float         previous = 1.0f;  // its own render opacity
    };
    std::vector<Hidden> hidden_;
};

namespace pass_opacity {

/// Before an overlay pass: only the seat's rows, markers, pins and command menu (0 player 1's) stay shown.
void ShowOnly(int seat) noexcept;

/// After the passes: every widget the four made transparent gets its own opacity back.
void RestoreAll() noexcept;

}  // namespace pass_opacity

}  // namespace lss::hud
