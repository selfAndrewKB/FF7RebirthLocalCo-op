#pragma once

// Which pass of the overlay render is running: player 2's (the mod's extra render, hud/second_image.h), then the
// game's own, player 1's; None outside the render. Set by root_overlay_render, the one driver of the passes; read by
// everything that paints into a menu-renderer slot during them (hud/shared_slot.h, hud/overlay_layout.cpp). Game
// thread only.

namespace lss::hud {

enum class OverlayPass : unsigned char
{
    None,
    PlayerTwo,
    PlayerOne,
};

namespace detail {
inline OverlayPass g_overlayPass = OverlayPass::None;
}

inline void SetOverlayPass(OverlayPass pass) noexcept
{
    detail::g_overlayPass = pass;
}

inline OverlayPass CurrentOverlayPass() noexcept
{
    return detail::g_overlayPass;
}

}  // namespace lss::hud
