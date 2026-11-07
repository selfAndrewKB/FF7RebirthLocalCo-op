#include "hud/passes/root_overlay_render.h"

#include "hud/battle/enemy_gauges.h"
#include "hud/battle/popups.h"
#include "hud/battle/target_views.h"
#include "hud/field/action_icon_base.h"
#include "hud/field/marker_timers.h"
#include "hud/field/prompt_rows.h"
#include "hud/hud_trace.h"
#include "hud/layout/hud_placement.h"
#include "hud/layout/overlay_layout.h"
#include "hud/minimap/minimap.h"
#include "hud/minimap/minimap_once.h"
#include "hud/minimap/seat_area_icons.h"
#include "hud/passes/column_hud_images.h"
#include "hud/passes/overlay_pass.h"
#include "hud/passes/pass_opacity.h"
#include "hud/passes/seat_hud_content.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "views/player_view_point.h"

#include <array>
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
bool                   promptRowsKnown = false;  // game thread: the seats' prompt rows are being followed
bool                   g_seatIn[players::kMaxPlayers] = {};  // game thread, by seat: in the game at the last render
bool                   g_installed = false;

// Render cost, logged every kRenderTimingFrames frames while a seat's image is on.
constexpr std::uint32_t kRenderTimingFrames = 240;
double        g_seatSeconds        = 0.0;
double        g_playerOneSeconds   = 0.0;
double        g_playerOneStart     = 0.0;
std::uint32_t g_renderTimedFrames  = 0;

using platform::frame_watch::Now;

using overlay_pass::SeatPassKind;
using PassKinds = std::array<SeatPassKind, players::kMaxPlayers>;  // by seat (index 0 unused)

/// What each seat's pass draws this frame (overlay_pass::PassKindOf over this frame's views), decided once before
/// the passes: the loop and AnotherPassBelow read the same answer. `splitShown`: a seat's own view on screen.
PassKinds PassKindsNow(bool splitShown) noexcept
{
    PassKinds kinds{};
    const bool  viewsSplit = views::ViewsSplit();
    const float aspectOne  = views::ViewAspect(0);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        kinds[static_cast<std::size_t>(seat)] =
            overlay_pass::PassKindOf(players::generation::Active(seat), splitShown, views::OnPlayerOne(seat), viewsSplit,
                                     views::ViewAspect(seat), aspectOne);
    }
    return kinds;
}

/// Whether a seat below `seat` is expected to have a pass after it this frame (the loop's own test without the
/// image's buffer check: a pass planned but not run is put back by SettleSeats after the loop).
bool AnotherPassBelow(int seat, const PassKinds& kinds) noexcept
{
    if (!ColumnHudImagesOn() || !SeatHudContentOn())
    {
        return false;
    }
    for (int below = seat - 1; below >= 1; --below)
    {
        if (kinds[static_cast<std::size_t>(below)] != SeatPassKind::None)
        {
            return true;
        }
    }
    return false;
}

/// The Info line when a seat's pass starts or stops drawing player 1's HUD for its column (by seat; game thread).
SeatPassKind g_loggedKind[players::kMaxPlayers] = {};

void NoteKind(int seat, SeatPassKind kind) noexcept
{
    SeatPassKind& logged = g_loggedKind[seat];
    if ((kind == SeatPassKind::PlayerOnes) == (logged == SeatPassKind::PlayerOnes))
    {
        logged = kind;
        return;
    }
    logged = kind;
    platform::Log(L"HUD: {}'s column {}", players::SeatName(seat),
                  kind == SeatPassKind::PlayerOnes
                      ? L"shows player 1's picture on a monitor of another shape than player 1's: its pass draws player 1's "
                        L"HUD laid out for it"
                      : L"no longer has a pass of player 1's HUD");
}

/// A seat's pass of the overlay render: its levers and updates, its render into the entry named for it, everything put
/// back for the next pass (the roots' contexts and player 1's re-run only after the last seat pass, `another` false).
/// Its image is ready (column_hud_images's ImageReady). `kind` PlayerOnes: the levers are the seat's (the layout and the
/// projections follow its view, which shows player 1's camera); the rows, markers, pins, command menu, cursors and the
/// minimap's map are player 1's. The area icons stay the seat's own instance, ticked under its view: they are the same
/// quest and party markers, and only that instance is placed for this view.
void SeatPass(int seat, bool another, SeatPassKind kind) noexcept
{
    const double deltaTime = BeginColumnImage();  // the engine's DeltaTime 0 for the render; the slot taking part
    overlay_pass::Set(seat);
    const int shown = kind == SeatPassKind::PlayerOnes ? 0 : seat;  // whose HUD the pass shows
    // that player's rows, markers, pins and command menu only (hud/passes/pass_opacity.h)
    pass_opacity::ShowOnly(shown);
    target_views::BeforePass(shown);  // the other players' cursors and gauges collapsed before the seat's updates
    const bool content = EnterSeat(seat);  // the seat's levers and updates (hud/passes/seat_hud_content.h)
    if (content)
    {
        seat_area_icons::Tick(seat, static_cast<float>(deltaTime));  // under the seat's view rect
    }
    seat_area_icons::ShowOnly(seat);
    const double start = Now();
    // The seat's minimap (hud/minimap/minimap.h); without it (and for player 1's HUD), player 1's pass alone renders
    // the minimap's map (hud/minimap/minimap_once.h).
    BeginMinimapSeatPass(minimap::BeginPaint(shown));
    RenderColumnImage();  // on a fault the minimap's target field and held slot are still put back below
    EndMinimapSeatPass();
    minimap::EndPaint();
    g_seatSeconds += Now() - start;
    enemy_gauges::EndSeatPass();  // each gauge's size back to player 1's before player 1's updates
    target_views::BeforePass(0);  // the seat's, collapsed before player 1's updates (put back after the render)
    if (content)
    {
        LeaveSeat(seat, another);
    }
    seat_area_icons::ShowOnly(0);
    pass_opacity::ShowOnly(0);  // player 1's rows, markers, pins and command menu for player 1's pass
    EndColumnImage();  // DeltaTime back
}

/// Replaces the call (entered from the game's code through a bare jump: nothing here may throw).
void RenderRootOverlay(void* application) noexcept
{
    const bool gameThread = players::generation::OnGameThread();
    // The whole call: the game's overlay render, the seats' passes before it, and the mod's work around it.
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::OverlayRender);
    if (gameThread)
    {
        TraceBeforeRender();  // the HUD trace (hud/hud_trace.h; a stub without the dev tools)
        target_views::BeginFrame();  // this frame's target lookups made afresh
        const bool splitShown = players::generation::AnyActive() && views::SplitShown();  // not player 1's single view
        // The field-action icons' base for the columns' layout (or the game's own), before the rows' layout below,
        // which then reads it.
        action_icon_base::Update(views::ViewsSplit() ? LastLayoutSize() : kSoloLayout);
        if (players::generation::AnyActive())
        {
            prompt_rows::Refresh();  // whose each prompt row is; each player's laid out on their own
            promptRowsKnown = true;
            for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
            {
                const bool in = players::generation::Active(seat);
                if (g_seatIn[seat] && !in)  // a seat out while another is in: its own HUD state forgotten
                {
                    prompt_rows::Forget(seat);
                    seat_area_icons::SeatOut(seat);
                    marker_timers::Forget(seat);
                    enemy_gauges::SeatOut(seat);
                }
                g_seatIn[seat] = in;
            }
        }
        else if (promptRowsKnown)
        {
            prompt_rows::Forget();
            seat_area_icons::AllOut();
            marker_timers::Forget();
            promptRowsKnown = false;
            for (bool& in : g_seatIn)
            {
                in = false;
            }
        }
        FollowHudPlacementBeforeRender();  // after the rows' layout, which can set a row's point
        // The seats' passes first, highest seat first, each whose image is on (hud/passes/column_hud_images.h
        // holds the slot and renders; the levers and the per-pass opacities around each render are the modules' own
        // calls, in SeatPass's order). A seat whose column shows player 1's camera (views/column_source.h
        // Source::PlayerOne: a held column while the game takes the whole screen, a shared ride, every seat's player
        // waiting or single-view on; or a lone waiting player) gets no pass while its view has player 1's shape: it
        // draws player 1's image, as a mirror's. On a monitor of another shape its pass draws player 1's HUD laid out
        // for it, so a menu or a subtitle shows whole there.
        overlay_pass::Set(overlay_pass::kNone);
        BeginFrame();
        minimap::BeginOverlayRender();
        BeginPasses();
        const PassKinds kinds = PassKindsNow(splitShown);
        for (int seat = players::kSeatsBuilt; seat >= 1; --seat)
        {
            const SeatPassKind kind = kinds[static_cast<std::size_t>(seat)];
            NoteKind(seat, kind);
            if (ImageReady(seat, kind != SeatPassKind::None, kind == SeatPassKind::PlayerOnes))
            {
                SeatPass(seat, AnotherPassBelow(seat, kinds), kind);
            }
        }
        if (PlayerOneDue())  // a pass planned after the last one that ran did not run: player 1's put back here,
        {                    // under the frame a pass holds
            HoldFrame();
            SettleSeats();
            ReleaseFrame();
        }
        BeginPlayerOnePass();  // the game's own call follows: whose entries were rendered, for the families
        overlay_pass::Set(0);
        g_playerOneStart = Now();
        if (players::generation::AnyActive() && !splitShown)
        {
            // Player 1's single view (players/single_view.h): no image for the seats, and player 1's
            // pass shows only player 1's own rows, marks, icons and command menu, as after a seat's pass. The
            // seats' rows stay painted (transparent), so their timers keep running.
            target_views::BeforePass(0);
            seat_area_icons::ShowOnly(0);
            pass_opacity::ShowOnly(0);  // the rows, markers, pins and the command menu (hud/passes/pass_opacity.h)
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
                    platform::Log(L"HUD image: overlay render ms per frame over {} frames: players 2 and 3's passes "
                                  L"{:.3f}, player 1's pass {:.3f}",
                                  g_renderTimedFrames, g_seatSeconds * perFrame, g_playerOneSeconds * perFrame);
                }
                g_seatSeconds       = 0.0;
                g_playerOneSeconds  = 0.0;
                g_renderTimedFrames = 0;
            }
        }
        EndFrame();                   // a slot player 1's pass did not take is submitted (logged)
        minimap::EndOverlayRender();  // the minimap's held pool-0 slot, if player 1's pass did not take it
        overlay_pass::Set(overlay_pass::kNone);
        enemy_gauges::EndFrame();  // the gauges' record, emptied for the next tick
        popups::EndFrame();
        pass_opacity::RestoreAll();  // every row, marker, pin and the command menu back to their own
        target_views::AfterPass();   // every mark hidden for player 1's pass back
        TraceAfterRender();
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
