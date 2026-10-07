#include "hud/root_overlay_render.h"

#include "dev/dev_tools.h"
#include "hud/action_icon_base.h"
#include "hud/area_icons_two.h"
#include "hud/enemy_gauges.h"
#include "hud/target_views.h"
#include "hud/hud_placement.h"
#include "hud/marker_timers.h"
#include "hud/overlay_layout.h"
#include "hud/popups.h"
#include "hud/field_markers.h"
#include "hud/prompt_rows.h"
#include "hud/second_image.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/command_menu_two.h"
#include "views/player_view_point.h"

#include <cstdint>
#include <span>

namespace lss::hud {

namespace {

// UGameEngine__Tick's call to the overlay render (RE-p2-hud.md P8.9; bytes read from the
// database): E8 rel32 at 0x1410A0156, inside the block at 0x1410A0150. Its target's prologue up
// to the stack probe call is checked before the call is redirected.
constexpr std::uintptr_t kRenderRootOverlayCall = 0x1410A0156;
constexpr std::uintptr_t kRenderRootOverlay     = 0x141627E98;  // void (FSlateApplication*)
constexpr std::uint8_t   kRenderRootOverlayPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};

// Game version 1.005 (RE-1005-hud-2.md): the call (0x1409F205E there) crosses a 16-byte block, so the target's entry is
// redirected instead (0x14167E258, first instruction `mov [rsp+8],rbx`, one block); UGameEngine::Tick's call is its only
// caller, so every call is still that one.
constexpr std::size_t kRenderRootOverlayFirst = 5;

using RenderFn = void (*)(void* application);

platform::CallRedirect  g_call;
platform::EntryRedirect g_entry;  // 1.005, in place of g_call
bool                   promptRowsKnown = false;  // game thread: player 2's prompt rows are being followed
bool                   g_installed = false;

/// Replaces the call (entered from the game's code through a bare jump: nothing here may throw).
void RenderRootOverlay(void* application) noexcept
{
    const bool gameThread = player_two::generation::OnGameThread();
    // The whole call: the game's overlay render, player 2's pass in it, and the mod's work around it.
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::OverlayRender);
    if (gameThread)
    {
        dev::BeforeOverlayRender();  // the HUD trace, in a build with the development tools
        target_views::BeginFrame();  // this frame's target lookups made afresh
        const bool splitShown = player_two::generation::Active() && views::SplitShown();  // not player 1's single view
        // The field-action icons' base for the split's layout (or the game's own), before the rows'
        // layout below, which then reads it.
        action_icon_base::Update(splitShown ? LastLayoutSize() : kSoloLayout);
        if (player_two::generation::Active())
        {
            prompt_rows::Refresh();  // whose each prompt row is; each player's laid out on their own
            promptRowsKnown = true;
        }
        else if (promptRowsKnown)
        {
            prompt_rows::Forget();
            area_icons_two::PlayerTwoOut();
            marker_timers::Forget();
            promptRowsKnown = false;
        }
        FollowHudPlacementBeforeRender();  // after the rows' layout, which can set a row's point
        BeginOverlayRender(splitShown);  // player 2's image first, if on
        if (player_two::generation::Active() && !splitShown)
        {
            // Player 1's single view (players/single_view.h): no image for player 2, and player 1's
            // pass shows only player 1's own rows, marks, icons and command menu, as after player 2's
            // pass. Player 2's rows stay painted (transparent), so their timers keep running.
            target_views::BeforePass(0);
            area_icons_two::ShowOnly(0);
            prompt_rows::ShowOnly(0);
            field_markers::ShowOnly(0);
            player_two::command_menu_two::ShowOnly(0);
        }
    }
    // The entry redirect's trampoline is set before its first call can arrive; only one of the two is ever installed.
    reinterpret_cast<RenderFn>(g_entry.Original() != 0 ? g_entry.Original() : g_call.Original())(application);
    if (gameThread)
    {
        EndOverlayRender();
        enemy_gauges::EndFrame();  // the gauges' record, emptied for the next tick
        popups::EndFrame();
        prompt_rows::Restore();  // every prompt row back to its own visibility
        field_markers::Restore();  // every point marker back to its own opacity
        player_two::command_menu_two::Restore();  // player 1's command menu back, player 2's hidden
        target_views::AfterPass();  // every mark hidden for player 1's pass back
        dev::AfterOverlayRender();
    }
}

}  // namespace

bool InstallRootOverlayRender(const platform::GameImage& image) noexcept
{
    const bool v1005  = platform::RunningBuild() == platform::BuildIndex::V1005;
    const auto site   = image.Rebase(kRenderRootOverlayCall, 5);
    const auto target = image.Rebase(kRenderRootOverlay);
    const auto first  = std::span(kRenderRootOverlayPrologue, kRenderRootOverlayFirst);
    if (!site || !target || !image.BytesMatch(kRenderRootOverlay, kRenderRootOverlayPrologue) ||
        !(v1005 ? g_entry.Install(*target, image.EntryInstruction(kRenderRootOverlay, first),
                                  reinterpret_cast<std::uintptr_t>(&RenderRootOverlay))
                : g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&RenderRootOverlay))))
    {
        platform::Log(L"HUD render hook: the overlay render call is not what the research read; widgets the game "
                      L"moves are placed on the next tick instead");
        return false;
    }
    g_installed = true;
    if (v1005)
    {
        platform::Log(L"HUD render hook: installed at the overlay render's entry (its call crosses a block in this build)");
    }
    return true;
}

void RemoveRootOverlayRender() noexcept
{
    if (g_installed && !(g_entry.IsInstalled() ? g_entry.Restore() : g_call.Restore()))
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
