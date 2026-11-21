#include "hud/passes/root_overlay_render.h"

#include "hud/battle/enemy_gauges.h"
#include "hud/battle/popups.h"
#include "hud/battle/target_views.h"
#include "hud/button_art.h"
#include "hud/field/action_icon_base.h"
#include "hud/field/marker_timers.h"
#include "hud/field/prompt_rows.h"
#include "hud/hud_roots.h"
#include "hud/hud_trace.h"
#include "hud/layout/hud_placement.h"
#include "hud/layout/overlay_layout.h"
#include "hud/lead_mark.h"
#include "hud/minimap/minimap.h"
#include "hud/minimap/minimap_once.h"
#include "hud/minimap/column_area_icons.h"
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

// UGameEngine__Tick's call to the overlay render (research/RE-p2-hud.md; bytes read from the
// database): E8 rel32, inside one 16-byte block. Its target's prologue up
// to the stack probe call is checked before the call is redirected.
constexpr std::uintptr_t kRenderRootOverlayCall = 0x1409F205E;
constexpr std::uint8_t   kRenderRootOverlayPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRenderRootOverlay{L"SlateApp_RenderRootOverlay", 0x14167E258,
                                                    kRenderRootOverlayPrologue};  // void (FSlateApplication*)

// Game version 1.005 (research/RE-1005-hud-2.md): the call crosses a 16-byte block, so the target's entry is
// redirected instead (first instruction `mov [rsp+8],rbx`, one block); UGameEngine::Tick's call is its only
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
using PassKinds = std::array<SeatPassKind, overlay_pass::kMaxColumns>;  // by column (index 0 unused)

/// The seat in a further column, or 0 when the column holds none (a mirror's, or not listed).
int SeatIn(int column) noexcept
{
    const int seat = overlay_pass::SeatAt(column);
    return players::SeatIndex(seat) ? seat : 0;
}

/// A mirror's column (views/mirror_player.h): a copy of player 1's picture with no seat.
bool MirrorIn(int column) noexcept
{
    return overlay_pass::SeatAt(column) == views::columns::kMirror;
}

/// What each further column's pass draws this frame (overlay_pass::PassKindOf over this frame's views, by the seat in
/// the column; MirrorPassKind for a mirror's, by its own view's shape), decided once before the passes: the loop and
/// AnotherPassBelow read the same answer. `splitShown`: a seat's own view on screen.
PassKinds PassKindsNow(bool splitShown) noexcept
{
    PassKinds   kinds{};
    const bool  viewsSplit = views::ViewsSplit();
    const float aspectOne  = views::ViewAspect(0);
    hud_roots::LocalPlayerSet byColumn{};
    const bool                listed = overlay_pass::Map().mirrors > 0 && hud_roots::LocalPlayers(byColumn);
    for (int column = 1; column < overlay_pass::kMaxColumns; ++column)
    {
        const int seat = SeatIn(column);
        if (seat == 0)
        {
            kinds[static_cast<std::size_t>(column)] =
                listed && MirrorIn(column)
                    ? overlay_pass::MirrorPassKind(viewsSplit,
                                                   views::ViewAspectOf(byColumn[static_cast<std::size_t>(column)]),
                                                   aspectOne)
                    : SeatPassKind::None;
            continue;
        }
        kinds[static_cast<std::size_t>(column)] =
            overlay_pass::PassKindOf(players::generation::Active(seat), splitShown, views::OnPlayerOne(seat), viewsSplit,
                                     views::ViewAspect(seat), aspectOne);
    }
    return kinds;
}

/// Whether a column below `column` is expected to have a pass after it this frame (the loop's own test without the
/// image's buffer check: a pass planned but not run is put back by SettleSeats after the loop).
bool AnotherPassBelow(int column, const PassKinds& kinds) noexcept
{
    if (!ColumnHudImagesOn() || !SeatHudContentOn())
    {
        return false;
    }
    for (int below = column - 1; below >= 1; --below)
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

/// The Info line when a mirror's column starts or stops having a pass of player 1's HUD (by column; game thread).
SeatPassKind g_loggedMirrorKind[overlay_pass::kMaxColumns] = {};

void NoteMirrorKind(int column, SeatPassKind kind) noexcept
{
    SeatPassKind& logged = g_loggedMirrorKind[column];
    if (kind == logged)
    {
        return;
    }
    logged = kind;
    platform::Log(L"HUD: the mirror view in column {} {}", column + 1,
                  kind == SeatPassKind::PlayerOnes
                      ? L"is on a monitor of another shape than player 1's: its pass draws player 1's HUD laid out for it"
                      : L"has no pass of its own (player 1's image fits it, or one view shows)");
}

/// A further column's pass of the overlay render, for the seat in it (0: a mirror's): its levers and updates, its render
/// into the entry named for the column, everything put back for the next pass (the roots' contexts and player 1's re-run
/// only after the last further pass, `another` false). Its image is ready (column_hud_images's ImageReady). `kind`
/// PlayerOnes, a copy: the layout and the projections follow the column's view, which shows player 1's camera; the rows,
/// markers, pins, command menu, cursors, gauges, minimap content and the area icons' range are player 1's, and no
/// player's window shows. The area icons are the column's own instance, placed for this view.
void ColumnPass(int column, int seat, bool another, SeatPassKind kind) noexcept
{
    const double deltaTime = BeginColumnImage();  // the engine's DeltaTime 0 for the render; the slot taking part
    overlay_pass::Set(column);
    const bool copy  = kind == SeatPassKind::PlayerOnes;  // a copy of player 1's picture
    const int  shown = copy ? 0 : seat;                    // whose HUD the pass shows
    // that player's rows, markers, pins and command menu only, and in a copy no player's window
    // (hud/passes/pass_opacity.h)
    pass_opacity::ShowOnly(shown, copy);
    target_views::BeforePass(shown);  // the other players' cursors and gauges collapsed before the column's updates
    button_art::EnterPass(shown);     // the shown player's button art, before the column's updates set texts
    const bool content = EnterSeat(column, seat, copy);  // the column's levers and updates (seat_hud_content.h)
    if (content)
    {
        // Under the column's marker host set: placed through the column's controller, ranged from the shown player's
        // character (player 1's: the game's own answer).
        column_area_icons::Tick(column, ColumnController(),
                                shown != 0 ? players::generation::MemberOf(shown) : nullptr,
                                static_cast<float>(deltaTime));
    }
    column_area_icons::ShowOnly(content ? column : 0);  // the levers refused: player 1's content, their icons too
    lead_mark::BeforeColumnPaint(column, content ? shown : 0);  // another player's kept-walk marker withheld from it
    const double start = Now();
    // The seat's minimap (hud/minimap/minimap.h); without it (and for player 1's HUD), player 1's pass alone renders
    // the minimap's map (hud/minimap/minimap_once.h).
    BeginMinimapSeatPass(minimap::BeginPaint(shown));
    RenderColumnImage();  // on a fault the minimap's target field and held slot are still put back below
    EndMinimapSeatPass();
    minimap::EndPaint();
    button_art::LeavePass();  // the game's art and player 1's states back before player 1's updates
    g_seatSeconds += Now() - start;
    enemy_gauges::EndSeatPass();  // each gauge's size back to player 1's before player 1's updates
    target_views::BeforePass(0);  // the seat's, collapsed before player 1's updates (put back after the render)
    if (content)
    {
        LeaveSeat(column, seat, another);
    }
    else
    {
        PutBackPlayerOneHostSet();  // the render wrote its own set; the passes after it and player 1's read player 1's
    }
    column_area_icons::ShowOnly(0);
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
                    marker_timers::Forget(seat);
                    enemy_gauges::SeatOut(seat);
                }
                g_seatIn[seat] = in;
            }
        }
        else if (promptRowsKnown)
        {
            prompt_rows::Forget();
            column_area_icons::AllOut();
            marker_timers::Forget();
            promptRowsKnown = false;
            for (bool& in : g_seatIn)
            {
                in = false;
            }
        }
        column_area_icons::FollowColumns(overlay_pass::Map());  // a column that changed hands starts its icons again
        FollowHudPlacementBeforeRender();  // after the rows' layout, which can set a row's point
        // The further columns' passes first, highest column first, each whose image is on (hud/passes/
        // column_hud_images.h holds the slot and renders; the levers and the per-pass opacities around each render are
        // the modules' own calls, in ColumnPass's order). A seat whose column shows player 1's camera (views/column_source.h
        // Source::PlayerOne: a held column while the game takes the whole screen, a shared ride, every seat's player
        // waiting or single-view on; or a lone waiting player) and a mirror's column get no pass while their view has
        // player 1's shape: they draw player 1's image. On a monitor of another shape their pass draws player 1's HUD
        // laid out for it, so a menu or a subtitle shows whole there.
        overlay_pass::Set(overlay_pass::kNone);
        BeginFrame();
        minimap::BeginOverlayRender();
        BeginPasses();
        const PassKinds kinds = PassKindsNow(splitShown);
        for (int column = overlay_pass::kMaxColumns - 1; column >= 1; --column)
        {
            const int          seat   = SeatIn(column);
            const bool         mirror = MirrorIn(column);
            const SeatPassKind kind   = kinds[static_cast<std::size_t>(column)];
            if (seat != 0)
            {
                NoteKind(seat, kind);
                g_loggedMirrorKind[column] = SeatPassKind::None;  // a mirror back in this column is noted again
            }
            else if (mirror)
            {
                NoteMirrorKind(column, kind);
            }
            const int shown = kind == SeatPassKind::PlayerOnes ? 0 : seat;  // whose HUD the pass shows
            if ((seat != 0 || mirror) && ImageReady(column, kind != SeatPassKind::None, shown))
            {
                ColumnPass(column, seat, AnotherPassBelow(column, kinds), kind);
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
            column_area_icons::ShowOnly(0);
            pass_opacity::ShowOnly(0);  // the rows, markers, pins and the command menu (hud/passes/pass_opacity.h)
        }
    }
    if (gameThread)
    {
        lead_mark::BeforePlayerOnePaint();  // the other players' kept-walk markers withheld from player 1's view
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
        marker_timers::EndFrame();  // a mirror's arrow timers forgotten where no mirror pass ran
        pass_opacity::RestoreAll();  // every row, marker, pin and the command menu back to their own
        target_views::AfterPass();   // every mark hidden for player 1's pass back
        TraceAfterRender();
    }
}

}  // namespace

bool InstallRootOverlayRender(const platform::GameImage& image) noexcept
{
    const bool entry  = platform::RunningBuild() != platform::BuildIndex::V1004;  // the call crosses a block
    const auto site   = image.Rebase(kRenderRootOverlayCall, 5);
    const auto target = platform::FindFunction(image, kRenderRootOverlay);
    const auto first  = kRenderRootOverlay.entryBytes.first(kRenderRootOverlayFirst);
    if (!site || !target ||
        !(entry ? g_entry.Install(*target, image.EntryInstruction(kRenderRootOverlay.address, first),
                                  reinterpret_cast<std::uintptr_t>(&RenderRootOverlay))
                : g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&RenderRootOverlay))))
    {
        platform::Log(L"HUD render hook: the overlay render call is not what the research read; widgets the game "
                      L"moves are placed on the next tick instead");
        return false;
    }
    g_installed = true;
    if (entry)
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
