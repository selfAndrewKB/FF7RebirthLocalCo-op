#include "hud/root_overlay_render.h"

#include "dev/dev_tools.h"
#include "hud/action_icon_base.h"
#include "hud/area_icons_two.h"
#include "hud/enemy_gauges.h"
#include "hud/target_views.h"
#include "hud/hud_placement.h"
#include "hud/marker_timers.h"
#include "hud/minimap.h"
#include "hud/minimap_once.h"
#include "hud/overlay_layout.h"
#include "hud/overlay_pass.h"
#include "hud/player_two_pass.h"
#include "hud/popups.h"
#include "hud/pass_opacity.h"
#include "hud/prompt_rows.h"
#include "hud/second_image.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "views/player_view_point.h"

#include <cstdint>
#include <span>

namespace lss::hud {

namespace {

// UGameEngine__Tick's call to the overlay render (RE-p2-hud.md P8.9; bytes read from the
// database): E8 rel32 at 0x1410A0156, inside the block at 0x1410A0150. Its target's prologue up
// to the stack probe call is checked before the call is redirected.
constexpr std::uintptr_t kRenderRootOverlayCall = 0x1410A0156;
constexpr std::uint8_t   kRenderRootOverlayPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRenderRootOverlay{L"SlateApp_RenderRootOverlay", 0x141627E98,
                                                    kRenderRootOverlayPrologue};  // void (FSlateApplication*)

// Game version 1.005 (RE-1005-hud-2.md): the call (0x1409F205E there) crosses a 16-byte block, so the target's entry is
// redirected instead (0x14167E258, first instruction `mov [rsp+8],rbx`, one block); UGameEngine::Tick's call is its only
// caller, so every call is still that one.
constexpr std::size_t kRenderRootOverlayFirst = 5;

using RenderFn = void (*)(void* application);

platform::CallRedirect  g_call;
platform::EntryRedirect g_entry;  // 1.005, in place of g_call
bool                   promptRowsKnown = false;  // game thread: player 2's prompt rows are being followed
bool                   g_installed = false;

// Render cost, logged every kRenderTimingFrames frames while player 2's image is on.
constexpr std::uint32_t kRenderTimingFrames = 240;
double        g_playerTwoSeconds   = 0.0;
double        g_playerOneSeconds   = 0.0;
double        g_playerOneStart     = 0.0;
std::uint32_t g_renderTimedFrames  = 0;

using platform::frame_watch::Now;

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
        // Player 2's pass first, if its image is on (hud/second_image.h holds the slot and renders; the levers and
        // the per-pass opacities around its render are the modules' own calls, in this order).
        SetOverlayPass(OverlayPass::None);
        BeginFrame();
        if (PlayerTwoImageReady(splitShown))
        {
            const double deltaTime = BeginPlayerTwoPass();  // the engine's DeltaTime 0 for the render; the slot held
            SetOverlayPass(OverlayPass::PlayerTwo);
            pass_opacity::ShowOnly(1);  // player 2's rows, markers, pins and command menu only (hud/pass_opacity.h)
            target_views::BeforePass(1);  // player 1's cursor and gauges collapsed before player 2's updates read them
            const bool content = EnterPlayerTwo();  // player 2's levers and updates (hud/player_two_pass.h)
            if (content)
            {
                area_icons_two::Tick(static_cast<float>(deltaTime));  // under player 2's view rect
            }
            area_icons_two::ShowOnly(1);
            const double start = Now();
            // Player 2's minimap (hud/minimap.h); without it, player 1's pass alone renders the minimap's map
            // (hud/minimap_once.h).
            BeginMinimapPlayerTwoPass(minimap::BeginPlayerTwoPaint());
            RenderPlayerTwo();  // on a fault the minimap's target field and held slot are still put back below
            EndMinimapPlayerTwoPass();
            minimap::EndPlayerTwoPaint();
            g_playerTwoSeconds += Now() - start;
            enemy_gauges::EndPlayerTwoPass();  // each gauge's size back to player 1's before player 1's updates
            target_views::BeforePass(0);  // player 2's, collapsed before player 1's updates (put back after the render)
            if (content)
            {
                LeavePlayerTwo();
            }
            area_icons_two::ShowOnly(0);
            pass_opacity::ShowOnly(0);  // player 1's rows, markers, pins and command menu for player 1's pass
            EndPlayerTwoPass();  // DeltaTime back
        }
        BeginPlayerOnePass();  // the game's own call follows: whether player 2's entry was rendered, for the families
        SetOverlayPass(OverlayPass::PlayerOne);
        g_playerOneStart = Now();
        if (player_two::generation::Active() && !splitShown)
        {
            // Player 1's single view (players/single_view.h): no image for player 2, and player 1's
            // pass shows only player 1's own rows, marks, icons and command menu, as after player 2's
            // pass. Player 2's rows stay painted (transparent), so their timers keep running.
            target_views::BeforePass(0);
            area_icons_two::ShowOnly(0);
            pass_opacity::ShowOnly(0);  // the rows, markers, pins and the command menu (hud/pass_opacity.h)
        }
    }
    // The entry redirect's trampoline is set before its first call can arrive; only one of the two is ever installed.
    reinterpret_cast<RenderFn>(g_entry.Original() != 0 ? g_entry.Original() : g_call.Original())(application);
    if (gameThread)
    {
        if (ImageThisFrame())
        {
            g_playerOneSeconds += Now() - g_playerOneStart;
            if (++g_renderTimedFrames >= kRenderTimingFrames)
            {
                if (platform::frame_watch::Detailed())  // the perf-log switch
                {
                    const platform::frame_watch::Job job(L"HUD image timing log");
                    const double perFrame = 1000.0 / g_renderTimedFrames;
                    platform::Log(L"HUD image: overlay render ms per frame over {} frames: player 2's pass {:.3f}, "
                                  L"player 1's pass {:.3f}",
                                  g_renderTimedFrames, g_playerTwoSeconds * perFrame, g_playerOneSeconds * perFrame);
                }
                g_playerTwoSeconds  = 0.0;
                g_playerOneSeconds  = 0.0;
                g_renderTimedFrames = 0;
            }
        }
        EndFrame();                   // a slot player 1's pass did not take is submitted (logged)
        minimap::EndOverlayRender();  // the minimap's held pool-0 slot, if player 1's pass did not take it
        SetOverlayPass(OverlayPass::None);
        enemy_gauges::EndFrame();  // the gauges' record, emptied for the next tick
        popups::EndFrame();
        pass_opacity::RestoreAll();  // every row, marker, pin and the command menu back to their own
        target_views::AfterPass();   // every mark hidden for player 1's pass back
        dev::AfterOverlayRender();
    }
}

}  // namespace

bool InstallRootOverlayRender(const platform::GameImage& image) noexcept
{
    const bool v1005  = platform::RunningBuild() == platform::BuildIndex::V1005;
    const auto site   = image.Rebase(kRenderRootOverlayCall, 5);
    const auto target = platform::FindFunction(image, kRenderRootOverlay);
    const auto first  = kRenderRootOverlay.entryBytes.first(kRenderRootOverlayFirst);
    if (!site || !target ||
        !(v1005 ? g_entry.Install(*target, image.EntryInstruction(kRenderRootOverlay.address, first),
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
