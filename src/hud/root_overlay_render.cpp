#include "hud/root_overlay_render.h"

#include "dev/dev_tools.h"
#include "hud/hud_placement.h"
#include "hud/second_image.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <cstdint>

namespace lss::hud {

namespace {

// UGameEngine__Tick's call to the overlay render (RE-p2-hud.md P8.9; bytes read from the
// database): E8 rel32 at 0x1410A0156, inside the block at 0x1410A0150. Its target's prologue up
// to the stack probe call is checked before the call is redirected.
constexpr std::uintptr_t kRenderRootOverlayCall = 0x1410A0156;
constexpr std::uintptr_t kRenderRootOverlay     = 0x141627E98;  // void (FSlateApplication*)
constexpr std::uint8_t   kRenderRootOverlayPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};

using RenderFn = void (*)(void* application);

platform::CallRedirect g_call;
bool                   g_installed = false;

/// Replaces the call (entered from the game's code through a bare jump: nothing here may throw).
void RenderRootOverlay(void* application) noexcept
{
    const bool gameThread = player_two::generation::OnGameThread();
    if (gameThread)
    {
        dev::BeforeOverlayRender();  // the HUD trace, in a build with the development tools
        FollowHudPlacementBeforeRender();
        BeginOverlayRender(player_two::generation::Active());  // player 2's image first, if on
    }
    reinterpret_cast<RenderFn>(g_call.Original())(application);
    if (gameThread)
    {
        EndOverlayRender();
        dev::AfterOverlayRender();
    }
}

}  // namespace

bool InstallRootOverlayRender(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kRenderRootOverlayCall, 5);
    const auto target = image.Rebase(kRenderRootOverlay);
    if (!site || !target || !image.BytesMatch(kRenderRootOverlay, kRenderRootOverlayPrologue) ||
        !g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&RenderRootOverlay)))
    {
        platform::Log(L"HUD render hook: the overlay render call is not what the research read; widgets the game "
                      L"moves are placed on the next tick instead");
        return false;
    }
    g_installed = true;
    return true;
}

void RemoveRootOverlayRender() noexcept
{
    if (g_installed && !g_call.Restore())
    {
        platform::Log(L"HUD render hook: changed after us; left in place, passing the call through");
    }
    g_installed = false;
}

bool RootOverlayRenderInstalled() noexcept
{
    return g_installed;
}

}  // namespace lss::hud
