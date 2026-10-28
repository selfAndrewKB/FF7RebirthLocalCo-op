#pragma once

// Whose player context each HUD root carries (hud/hud_roots.h), so a switch calls the game's setter only on the roots
// whose player changes. The further seats' passes run highest seat first, then player 1's (hud/passes/
// root_overlay_render.cpp), and the roots go back to player 1 only once, after the last further pass: with three
// players a root shown in both further passes goes player 1's -> player 3's -> player 2's -> player 1's, three calls
// a frame instead of four. Each call walks the root's whole tree of user widgets (UUserWidget::SetPlayerContext,
// stock UE4), which is what the HUD's switch costs. Pure: host-tested (tests/context_owners_test.cpp).

#include <cstdint>

namespace lss::hud::context_owners {

/// The seat whose context a root carries; 0 is player 1's (every root's between frames).
using Owner = std::int8_t;

/// Toward a further seat: the root is set when the seat's pass needs it (shown in that pass, or self-placing) and does
/// not carry the seat's context already. A root the pass does not need keeps what it carries: that pass neither ticks
/// nor paints it.
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
