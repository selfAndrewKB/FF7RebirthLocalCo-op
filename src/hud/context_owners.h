#pragma once

// Whose player context each HUD root carries (hud/hud_roots.h), so a switch calls the game's setter only on the roots
// whose player changes. The further seats' passes run highest seat first, then player 1's (hud/passes/
// root_overlay_render.cpp), and the roots go back to player 1 only once, after the last further pass: with three
// players a root shown in both further passes goes player 1's -> player 3's -> player 2's -> player 1's, three calls
// a frame instead of four. Each call walks the root's whole tree of user widgets (UUserWidget::SetPlayerContext,
// stock UE4), which is what the HUD's switch costs; the roots whose trees never read the context are not switched at
// all. Pure: host-tested (tests/context_owners_test.cpp).

#include <array>
#include <cstdint>
#include <string_view>

namespace lss::hud::context_owners {

/// The seat whose context a root carries; 0 is player 1's (every root's between frames).
using Owner = std::int8_t;

/// Root classes whose whole tree never reads the player context while it ticks or paints: the battle command menu and
/// the party status, which take their member and party from their own command controller (RE-hud-context-readers.md,
/// static). Exact Blueprint class names; extend only with the same evidence per class.
inline constexpr std::array<std::wstring_view, 2> kContextFreeClasses{L"Command_C", L"Status_C"};

/// The root's class is on the list above.
constexpr bool IsContextFree(std::wstring_view className) noexcept
{
    for (const std::wstring_view name : kContextFreeClasses)
    {
        if (name == className)
        {
            return true;
        }
    }
    return false;
}

/// Whether a further seat's pass needs the root to carry its context: shown in that pass, or self-placing, and not a
/// context-free class while the skip is on (such a root keeps player 1's all frame).
constexpr bool NeededBySeat(bool shown, bool selfPlacing, bool contextFree, bool skipContextFree) noexcept
{
    return (shown || selfPlacing) && !(contextFree && skipContextFree);
}

/// Toward a further seat: the root is set when the seat's pass needs it (shown in that pass, or self-placing) and does
/// not carry the seat's context already. A root the pass does not need keeps what it carries: that pass neither ticks
/// nor paints it, or it is of a context-free class, which reads no context while it ticks or paints.
constexpr bool SetForSeat(Owner owner, int seat, bool needed) noexcept
{
    return needed && owner != seat;
}

/// Back to player 1, before player 1's updates and pass: every root a further seat's context reached.
constexpr bool SetForPlayerOne(Owner owner) noexcept
{
    return owner != 0;
}

}  // namespace lss::hud::context_owners
