// LocalSplitScreen: the UE4SS entry point.
//
// This file wires the modules together:
//   1. verify every engine fact the mod relies on (facts/engine.h); if any fails, install nothing;
//   2. if all hold, install the renderer (one view family per player), the session (each seat's join and leave) and
//      its stages, then the module table in order (modules.h), the dev tools and the engine callbacks;
//   3. tick the modules once per engine tick, outside Draw, in two phases around the session's tick (modules.h);
//      at unload, remove in reverse.
// Design: Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md

#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "dev/dev_tools.h"
#include "engine_tick.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "feedback/seat_ffb_managers.h"
#include "feedback/traversal_rumble.h"
#include "hud/layout/hud_placement.h"
#include "hud/layout/overlay_layout.h"
#include "input/climb_context.h"
#include "input/pad_sticks.h"
#include "modules.h"
#include "platform/code_memory.h"
#include "platform/frame_watch.h"
#include "platform/game_folder_tools.h"
#include "platform/image.h"
#include "platform/level_names.h"
#include "platform/log.h"
#include "platform/memory_watch.h"
#include "platform/module_folder.h"
#include "platform/settings.h"
#include "platform/thread_times.h"
#include "platform/verified_code.h"
#include "players/console_access.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/join_keys.h"
#include "players/seats.h"
#include "players/single_view.h"
#include "players/split_layout.h"
#include "players/split_screen_settings.h"
#include "players/split_settings_access.h"
#include "players/tracked_object.h"
#include "players/weak_object.h"
#include "renderer/additional_family_output.h"
#include "renderer/async_compute.h"
#include "renderer/family_boundary.h"
#include "renderer/residency_off.h"
#include "renderer/view_family_submission.h"
#include "rides/shared_ride.h"
#include "session/field_exit.h"
#include "session/lead_gate.h"
#include "session/player_window.h"
#include "session/session.h"
#include "views/column_layout.h"
#include "views/column_source.h"
#include "views/display_align.h"
#include "views/mirror_player.h"
#include "views/player_view_point.h"
#include "views/whole_screen.h"
#include "views/window_span.h"
#include "world/charapop_centre.h"
#include "world/charapop_near.h"
#include "world/world_actor_reach.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <optional>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

namespace Hook = RC::Unreal::Hook;
using lss::platform::Log;

constexpr const wchar_t* kVersion = L"1.3.0";  // built against the Nexus UE4SS (c0a8d4c)

/// How often the seats' status lines are written while a seat is in.
constexpr ULONGLONG kStatusIntervalMs = 10'000;

/// Checks every startup fact against the running game; each failure is logged with the fact's name.
/// Returns the live address of RemoveLocalPlayer when all hold.
std::optional<std::uintptr_t> VerifyEngine(const lss::platform::GameImage& image) noexcept
{
    bool allHold = true;
    for (const auto& fact : lss::facts::kStartupFacts)
    {
        if (!lss::platform::FindFunction(image, fact))
        {
            allHold = false;
        }
    }

    const auto entryHolds = image.ReadPointer(lss::facts::kBeginRenderingViewFamilyEntry);
    const auto engineFn   = image.Rebase(lss::facts::kBeginRenderingViewFamily);
    if (!entryHolds || !engineFn || *entryHolds != *engineFn)
    {
        Log(L"check failed: the renderer's vtable entry does not hold BeginRenderingViewFamily "
            L"(a game update, or another mod already replaced it)");
        allHold = false;
    }

    // The GetViewPoint entry is not checked here: the split screen works without it (with bars),
    // and its install is itself a compare-and-swap against the expected function
    // (views/player_view_point).

    const auto removeLocalPlayer = image.Rebase(lss::facts::kRemoveLocalPlayer.address);
    if (!allHold || !removeLocalPlayer)
    {
        return std::nullopt;
    }
    return removeLocalPlayer;
}

/// Player 1's controller while it is a live object: the world context for the viewport's split switch.
RC::Unreal::UObject* PlayerOneWorldContext() noexcept
{
    auto* controller = static_cast<RC::Unreal::UObject*>(lss::players::generation::PlayerOneController());
    return lss::players::IsLiveObject(controller) ? controller : nullptr;
}

std::optional<bool> ReadSplitSwitch() noexcept
{
    return lss::players::split_settings::ForceDisabled(PlayerOneWorldContext());
}

bool WriteSplitSwitch(bool disabled) noexcept
{
    return lss::players::split_settings::SetForceDisabled(PlayerOneWorldContext(), disabled);
}

// The two-player layout is one fixed field: its held setting's name is for the log only.
std::optional<std::uint8_t> ReadTwoPlayerLayout(const wchar_t*) noexcept
{
    return lss::players::split_settings::TwoPlayerLayout();
}

bool WriteTwoPlayerLayout(const wchar_t*, std::uint8_t value) noexcept
{
    return lss::players::split_settings::SetTwoPlayerLayout(value);
}

class LocalSplitScreenMod;

/// The mod while it is set up, for the loader's map-load callback: UE4SS c0a8d4c keeps a callback for the session (no
/// unregister), so it reads the mod through here and does nothing once the mod is gone.
std::atomic<LocalSplitScreenMod*> g_instance{nullptr};

class LocalSplitScreenMod final : public RC::CppUserModBase
{
public:
    LocalSplitScreenMod()
    {
        ModName        = STR("LocalSplitScreen");
        ModVersion     = kVersion;  // one version string: the one the log's "starting" line prints
        ModDescription = STR("Two-player split-screen, one view family per player");
        ModAuthors     = STR("Andrew");
        // Before the engine creates its D3D12 device, which reads both settings once.
        lss::renderer::ApplyResidencyOff();
        lss::renderer::ApplyAsyncComputeOn();
    }

    ~LocalSplitScreenMod() override
    {
        g_instance.store(nullptr, std::memory_order_release);  // the map-load callback finds no mod from now on
        lss::engine_tick::Stop();

        // With a seat still in, the engine keeps rendering several views and calling the seats'
        // wrappers. Putting the engine's own submission back would render them in one family, the
        // configuration this mod exists to avoid, and a seat's camera manager depends on its
        // wrappers. Removing a player here is not an option: the destructor may run off the game
        // thread. So everything stays installed; the DLL is pinned, so it stays valid. The characters
        // raised for the seats' views are given back either way (that setter runs from any thread), and the NPC
        // centre when this runs on the game thread.
        lss::world::charapop_near::Release();
        lss::world::charapop_centre::Release();
        lss::world::world_actor_reach::Release();
        // The monitors the span lined up go back for the Windows session (plain Win32, any thread); the window itself
        // is the engine's, reshaped back only on the game thread.
        if (lss::players::generation::OnGameThread())
        {
            lss::views::window_span::Leave(L"the mod unloads");
        }
        lss::views::display_align::Restore();
        if (session_ && (session_->AnyIn() || lss::views::mirror_player::Count() > 0))
        {
            Log(L"unloading with player 2, player 3 or a mirror view still in: the per-player rendering and the stages "
                L"stay installed");
            return;
        }
        // The reverse of the install order: the field-exit catch and the dev tools (installed after the table),
        // the table (modules.cpp walks it backwards), the session's stages, the renderer.
        lss::session::field_exit::Remove();
        lss::dev::Remove();
        lss::modules::Remove();
        if (session_)
        {
            session_->RemoveStages();
        }
        if (rendererInstalled_)
        {
            lss::renderer::RemoveViewFamilySubmission();
            lss::renderer::RemoveAdditionalFamilyOutput();
        }
    }

    auto on_unreal_init() -> void override
    {
        Log(L"v{} starting", kVersion);
        // A diagnosis aid: `log-level.txt` in the mod's folder (one word: error, warning, info, debug or trace) sets the
        // level before the startup checks run, which the control command `log-level` can only change afterwards.
        if (std::wifstream level(lss::platform::ModFolder() / L"log-level.txt"); level)
        {
            std::wstring word;
            level >> word;
            if (const auto parsed = lss::platform::ParseLevel(word))
            {
                lss::platform::SetLevel(*parsed);
                Log(L"log level {} (log-level.txt)", lss::platform::LevelWord(*parsed));
            }
        }
        // The settings file (platform/settings.h), read once and written back from the table before any owner
        // installs: each owner applies its option at its install.
        lss::platform::settings::Load();
        // Recognises the game build (and so every PerBuild value) before any thread of ours can read one; the later
        // Locate calls find the same build.
        const auto image = lss::platform::GameImage::Locate();
        if (image)
        {
            lss::renderer::ReportResidency(*image);
            lss::renderer::ReportAsyncCompute(*image);
        }
        lss::platform::memory_watch::Start(image);  // its own thread, from launch to exit

        // A measuring control: with `watch_only.txt` in the mod's folder, nothing is installed but the
        // frame-hitch watch, so the game's own frame times can be compared with the mod's.
        // `perf_log.txt` turns the perf log on from launch (the frame-time and per-thread CPU lines every 10 s), for
        // machines without the control scripts.
        if (lss::platform::FlagFile(L"perf_log.txt"))
        {
            lss::platform::frame_watch::SetDetailed(true);
            Log(L"perf log on (perf_log.txt)");
        }
        if (lss::platform::WatchOnly())
        {
            Log(L"watch only (watch_only.txt): nothing installed except the frame-hitch watch; the game runs unmodified");
            lss::platform::frame_watch::SetDetailed(true);  // the hitch summary is this control's whole output
            // The console alone (no engine redirects), so a script can still quit the game.
            if (const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt)
            {
                session_.emplace(*removeLocalPlayer);
#if LSS_DEV
                // Nothing is installed to drive: the console and quit alone (no join on a session without stages).
                try
                {
                    lss::players::console::RegisterCommands([this] { return session_->AnyIn(); });
                }
                catch (...)
                {
                    Log(L"control: registering the console's commands failed (out of memory?)");
                }
#endif
                lss::dev::InstallControl(*session_);
            }
            if (image && lss::engine_tick::Install(*image))
            {
                lss::engine_tick::Start(
                    [](void*) {
                        lss::platform::frame_watch::OnFrameStart();
                        lss::dev::TickBeforeSession();
                    },
                    nullptr);
            }
            return;
        }

        if (image && image->Build() != nullptr)
        {
            Log(L"game version {}{}", image->Build()->name,
                image->Build()->source ? L" (the source build)" : L" (addresses translated from the source build)");
        }
        lss::platform::LogGameFolderTools();  // the other tools that change what the mod sees, named in the log
        const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt;
        // The heartbeat first (it passes the tick through until Start, below): without it nothing of the mod runs,
        // so a failed check stands the whole mod aside like any other startup fact.
        if (!removeLocalPlayer || !lss::engine_tick::Install(*image))
        {
            Log(L"standing aside: nothing installed, the game runs unmodified");
            return;
        }
        // The game's end-of-frame state reset between two families (the main-menu crash's fix),
        // before the submission that asks for it; logs its own outcome.
        lss::renderer::InstallFamilyBoundary(*image);
        if (!InstallRenderer(*image))
        {
            Log(L"standing aside: nothing installed, the game runs unmodified");
            return;
        }
        lss::players::weak_object::Install(*image);    // the engine's weak pointers, once, for every module
        // The game's shared globals, once, for every module (a missing one is logged; its readers stand aside).
        lss::players::game_globals::Install(*image);
        lss::facts::InstallInBattle(*image);  // the game's in-battle test, once, for the stages that ask it
        session_.emplace(*removeLocalPlayer);
        session_->InstallStages(*image);
        // A seat's join takes the mirror views out right before its CreatePlayer (views/mirror_player.h).
        session_->SetClearMirrors([]() noexcept { return lss::views::mirror_player::Clear(L"a player joins"); });
        lss::modules::Install(*image);  // every other module, in table order (modules.cpp); each logs its own line
        lss::session::player_window::SetSession(&*session_);  // the windows' joins and leaves go through the session
        lss::players::ChooseFromSettings(layout_);  // the two-player layout the file names, for the first join

        // The options the Co-op page sets, in every build (the rest register at their owner's install).
        lss::modules::RegisterOptions(layout_);
        // The dev tools (built with LSS_DEV_TOOLS only): the console, the HUD trace and probe, and the watches
        // that only log (dev/dev_tools.h names them); a player's build has none of them. Every control command is
        // registered first, each owner's (modules.cpp lists them), so the console opens on the whole table.
#if LSS_DEV
        lss::modules::RegisterCommands(*session_, singleViewChoice_);
#endif
        lss::dev::Install(*image, *session_);

        // The seats' players leave before any map load: the new world would log them in again, and their
        // camera managers' fixed-name spawns would collide with player 1's outside a join. The loader's callback
        // answers {false, false}: the game's LoadMap runs as it is. (No map load has been seen in play: the title's
        // Continue, a save load, a retry and chapter select all leave the field inside the one world.)
        Hook::RegisterLoadMapPreCallback([](RC::Unreal::UEngine*, RC::Unreal::FWorldContext&, RC::Unreal::FURL,
                                            RC::Unreal::UPendingNetGame*, RC::Unreal::FString&) {
            if (LocalSplitScreenMod* mod = g_instance.load(std::memory_order_acquire))
            {
                mod->LeaveBeforeWorldChange(L"a map is loading", true);
            }
            return std::pair<bool, bool>{false, false};
        });
        // The same for the seats before a retry, reload or load leaves the field inside this world (no map load);
        // the mirror views stay in through it.
        if (!lss::session::field_exit::Install(
                *image,
                [](void* self) {
                    auto* mod = static_cast<LocalSplitScreenMod*>(self);
                    mod->LeaveBeforeWorldChange(L"the game leaves the field", false);
                },
                this))
        {
            Log(L"a retry, reload or load is caught only by the engine tick's check of the field running byte");
        }

        // The heartbeat last, once everything it reaches is in place (this runs on the loader's thread; the ticks on
        // the game's).
        g_instance.store(this, std::memory_order_release);
        lss::engine_tick::Start([](void* self) { static_cast<LocalSplitScreenMod*>(self)->OnEngineTick(); }, this);

        Log(L"ready: every engine check passed. Ctrl+F7 adds a player (2, then 3), Ctrl+F8 removes the last one in, Ctrl+F5 switches "
            L"top/bottom and side by side, Ctrl+F4 switches filled halves and 16:9 halves with bars. Players 2 and 3 start on "
            L"their own camera and pad; for testing, Ctrl+F6 switches their views to player 1's camera and Ctrl+F9 "
            L"merges the pads. Ctrl+F2 spans the window across the monitor row, one monitor per player; Ctrl+F3 "
            L"switches the three-player monitor mode (three monitors, player 1's picture in the empty columns)");
        const auto pool = lss::platform::CodePool();
        Log(L"code pool: {} piece(s), {} bytes of placed code in {} block(s) of 64 KiB (one search each)", pool.pieces,
            pool.bytes, pool.blocks);
    }

private:
    /// The two renderer changes, both or neither. The output rule goes first: with it alone, one
    /// player renders exactly as the game renders (the game's single family is never marked additional), so
    /// there is never a moment when families are split but a later one still clears the screen.
    bool InstallRenderer(const lss::platform::GameImage& image) noexcept
    {
        if (!lss::renderer::InstallAdditionalFamilyOutput(image))
        {
            Log(L"could not install the final output's load-action rule");
            return false;
        }
        if (!lss::renderer::InstallViewFamilySubmission(image))
        {
            lss::renderer::RemoveAdditionalFamilyOutput();
            return false;
        }
        rendererInstalled_ = true;
        // The perf log's window (platform/frame_watch.h): after the renderer's frame-time report, the frame threads'
        // CPU time (the census, a dev tool, registers its own).
        lss::platform::frame_watch::AddWindowReport(
            [](std::uint32_t frames, double, bool log) { lss::platform::thread_times::EndWindow(frames, log); });
        Log(L"renderer: one view family per player; later families keep the screen (stock output rule restored)");
        return true;
    }

    /// The seats' players leave before the world changes under them (a map load, or the game leaving the field
    /// for a retry, reload or load); the session joins again once the new field is ready. The mirror views leave
    /// only for a map load (`mapLoad`): their controllers are this world's, and a new map would spawn them again
    /// outside a join. Through a field exit inside the world they stay, so the loading screen is drawn in every
    /// column (views/mirror_player.h; the field's re-entry leaves a controller without a member alone,
    /// research/RE-mirror-loads.md).
    void LeaveBeforeWorldChange(const wchar_t* why, bool mapLoad) noexcept
    {
        if (mapLoad)
        {
            lss::modules::ClearMirrorsForMapLoad(why);
        }
        else if (const int mirrors = lss::views::mirror_player::Count(); mirrors > 0)
        {
            Log(L"{}: {} mirror view(s) stay in through the load (the loading screen is drawn in every column)", why,
                mirrors);
        }
        // The field goes with its level, players in or not: a level entered again is judged from its own data.
        lss::session::lead_gate::ForgetAll();
        if (!session_->AnyIn())
        {
            return;
        }
        const bool left = session_->LeaveNow();
        // Now, not at the next tick: the view points must not keep the seats' old players; every column is its
        // player's own again (the next tick decides afresh).
        FollowShown(lss::views::column_source::Shown{});
        lss::views::Follow(SplitFrameNow(false, false));
        // The viewport outlives the world: its split switch is cleared while player 1's controller
        // is still this world's.
        LogSingleViewChange(singleView_.Update(false));
        if (left)
        {
            Log(L"{}: players 2 and 3 left (and join again once the field is ready)", why);
        }
        else
        {
            Log(L"WARNING: {} and player 2 or 3 could not be removed; a new world would log them in, and their "
                L"camera managers' spawns are expected to collide with player 1's",
                why);
        }
    }

    /// Runs on the game thread before each engine tick, outside Draw.
    void OnEngineTick() noexcept
    {
        lss::platform::frame_watch::OnFrameStart();
        const lss::platform::frame_watch::Charge charge(lss::platform::frame_watch::Part::EngineTick);
        lss::modules::TickBeforeSession();  // the tick's identities, then each module's work before the keys
        if (!tickSeen_)
        {
            tickSeen_ = true;
            Log(L"engine tick callback is live");
        }
        switch (keys_.Poll())
        {
            case lss::players::PlayerIntent::Join:
                // The first player not in joins (player 2, then player 3; one waiting to join is asked again); the
                // join logs its own outcome.
                if (const int seat = lss::modules::SeatToJoin(*session_); seat != 0)
                {
                    session_->Join(seat);
                }
                else
                {
                    Log(L"Ctrl+F7: every player is already in");
                }
                break;
            case lss::players::PlayerIntent::Leave:
                // The last player in leaves (player 3, then player 2); a wait to join counts as in.
                if (const int seat = lss::modules::SeatToLeave(*session_); seat != 0)
                {
                    lss::modules::LeaveSeat(*session_, seat, L"a player leaves (Ctrl+F8)");  // logs its own outcome
                }
                break;
            case lss::players::PlayerIntent::ToggleMonitorMode:
                LogAction(L"three-player monitor mode (Ctrl+F3)",
                          lss::modules::SetMonitorMode(!lss::views::window_span::Held() &&
                                                       !lss::views::window_span::WaitsForMenu()));
                break;
            case lss::players::PlayerIntent::ToggleWindowSpan:
                LogAction(L"window span (Ctrl+F2)",
                          lss::modules::SetWindowSpan(*session_, !lss::views::window_span::In(), L"Ctrl+F2"));
                break;
            case lss::players::PlayerIntent::ToggleOwnCamera:
                LogAction(L"camera choice (Ctrl+F6)",
                          lss::modules::SetOwnCamera(*session_, !lss::views::ShowsOwnCamera()));
                break;
            case lss::players::PlayerIntent::ToggleLayout: LogLayoutChange(layout_.Toggle()); break;
            case lss::players::PlayerIntent::ToggleFill:
                LogAction(L"split halves (Ctrl+F4)", lss::modules::SetFill(!lss::views::FillsHalves()));
                break;
            case lss::players::PlayerIntent::TogglePadSticks: session_->TogglePadSticks(); break;
            case lss::players::PlayerIntent::ToggleNpcDetail:
                if (lss::world::charapop_near::Installed())
                {
                    lss::world::charapop_near::SetDetail(
                        lss::world::charapop_near::NextDetail(lss::world::charapop_near::CurrentDetail()));
                }
                break;
            case lss::players::PlayerIntent::ToggleNpcCentre:
                if (lss::world::charapop_centre::Installed())
                {
                    lss::world::charapop_centre::SetEnabled(!lss::world::charapop_centre::Enabled());
                }
                break;
            case lss::players::PlayerIntent::None: break;
        }
        lss::session::field_exit::Watch();
        if (session_->AnyIn() && lss::session::field_exit::FieldLeft())
        {
            LeaveBeforeWorldChange(L"the field stopped running (a field exit the redirected entry did not see)", false);
        }
        lss::dev::TickBeforeSession();
        // The buggy and the bronco (rides/shared_ride.h): every seat stands aside, the game's merge reaches
        // player 1 from every pad.
        const bool shared = lss::rides::shared_ride::Tick();
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)  // each seat with its own force-feedback manager
        {
            lss::feedback::traversal_rumble::CopyPlayerOne(seat,
                                                            shared && lss::feedback::seat_ffb_managers::Attached(seat));
        }
        session_->SetSharedRide(shared);
        session_->SetColumnsHeld(columnsHeld_);  // as the last columns' tick read it (the columns follow the session)
        lss::input::pad_sticks::SetStoodDown(shared);
        lss::input::climb_context::Tick();  // this frame's input copies for the climbs on the anim worker (W5)
        lss::session::player_window::RunRequests();  // the pads' joins and leaves, before the session's tick (as the keys')
        session_->Tick();
        // The columns right after the session: the mirror views toward them, the window span to them.
        const lss::modules::Columns columns = lss::modules::TickColumns();
        columnsListed_                      = columns.map.count;
        columnsHeld_                        = columns.held;
        firstPlayer_                        = columns.firstPlayer;
        lss::dev::TickAfterSession();
        // Where each column's view comes from this tick (views/column_source.h decides; this is the one place that
        // applies it): the engine's single view through its switch (players/single_view.h), a seat's view on
        // player 1's camera through the view points, and its HUD pass skipped, through the frame.
        lss::views::column_source::Moment moment;
        moment.columnsHeld = columns.held;
        moment.sharedRide  = shared;
        moment.choice      = singleViewChoice_;
        moment.map         = columns.map;
        bool anySeatIn = false;
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            const auto state = session_->State(seat);
            moment.seats[static_cast<std::size_t>(seat)] = state;
            anySeatIn = anySeatIn || lss::players::InGame(state);
        }
        // Read only when it can matter (the read logs each change of reason).
        moment.wholeScreen = anySeatIn && moment.choice == lss::views::column_source::Override::Auto &&
                             lss::views::WholeScreenShown();
        lss::views::column_source::Shown shown = lss::views::column_source::Decide(moment);
        LogSingleViewChange(singleView_.Update(shown.Single()));
        if (shown.Single() && !singleView_.Single())
        {
            // The switch could not be set (or read): the columns stay on screen, so the same moment is answered as
            // with columns held: player 1's camera and HUD in every other column (never a split with a menu over it).
            moment.columnsHeld = true;
            shown              = lss::views::column_source::Decide(moment);
        }
        FollowShown(shown);
        // A switch found set, or one the mod could not clear, is what is on screen: nothing per player shows then.
        const bool splitShown = shown.SplitShown() && !singleView_.Single();
        const bool viewsSplit = shown.ViewsSplit() && !singleView_.Single();
        lss::modules::Frame frame;
        frame.shown      = shown;
        frame.splitShown = splitShown;
        frame.viewsSplit = viewsSplit;
        frame.columns    = columns.map.count;
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            frame.seats[static_cast<std::size_t>(seat)].state = moment.seats[static_cast<std::size_t>(seat)];
        }
        lss::modules::TickAfterSession(frame);
        // The players' windows (session/player_window.h): the pads' presses this tick, each window in its own view.
        lss::session::player_window::Frame windows;
        windows.screens = columns.map.count;
        windows.single  = singleView_.Single();
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            windows.seatIds |= moment.seats[static_cast<std::size_t>(seat)] != lss::players::SeatState::Out ? 1u << seat : 0u;
        }
        lss::session::player_window::Tick(windows);
        lss::dev::TickWatches(splitShown);  // the dev watches, after the levers they observe
        // The settings several views need, while more than one view family renders (a seat's view or a mirror view).
        settings_.Update(columns.map.count > 1, [](const std::wstring& line) { Log(L"{}", line); });
        // The layout by column count: the player's two-player choice with two local players (a seat's player, or a
        // mirror beside player 1), side by side whatever the choice while the window spans monitors; the three
        // columns with three, and on a span each column its monitor's width (views/column_layout.h).
        const bool span = lss::views::window_span::In();
        if (span != spanWas_)
        {
            spanWas_ = span;
            Log(L"split layout: {}", span ? L"the window spans monitors: two players side by side, one per monitor"
                                           : L"one window again: two players as chosen");
        }
        LogLayoutChange(layout_.Update(columns.map.count == 2, span));
        // Each place of the engine's list as the screen order sees it: the seat whose view it is, -1 for a mirror's.
        lss::views::column_rules::ListSeats seats{-1, -1, -1};
        for (int place = 0; place < static_cast<int>(seats.size()) && place < columns.map.count; ++place)
        {
            const int seat = columns.map.seatAt[static_cast<std::size_t>(place)];
            seats[static_cast<std::size_t>(place)] = seat >= 0 && seat < lss::players::kMaxPlayers ? seat : -1;
        }
        lss::views::column_layout::Tick(columns.map.count, lss::views::window_span::ColumnWidths(),
                                        lss::views::window_span::ColumnHeights(), seats, columns.held);
        // After the layout: the views' shapes read it; the one place "the split is shown" is kept (views::SplitShown,
        // views::ViewsSplit).
        lss::views::Follow(SplitFrameNow(splitShown, viewsSplit));
        lss::hud::UpdateHudPlacement(viewsSplit && lss::hud::BandEnabled());

        const ULONGLONG now = GetTickCount64();
        if (!session_->AnyIn() || !lss::platform::frame_watch::Detailed())  // perf-log: the periodic status
        {
            nextStatus_ = now + kStatusIntervalMs;
        }
        else if (now >= nextStatus_)
        {
            nextStatus_ = now + kStatusIntervalMs;
            if (lss::platform::Enabled(lss::platform::Level::Debug))  // periodic: a Debug report (the status command writes it at any level)
            {
                session_->LogStatus();
            }
        }
    }

    /// Each column's source as last decided (FollowShown keeps it): each seat's change is logged once, with its
    /// reason, while the seat is in the game (a seat that left logs its leave, not a view it no longer has).
    void FollowShown(const lss::views::column_source::Shown& shown) noexcept
    {
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            if (!shown.seatIn[at])
            {
                continue;
            }
            if (const auto text = lss::views::column_source::Describe(seat, shown_.seats[at], shown.seats[at]);
                !text.empty())
            {
                Log(L"{}", text);
            }
        }
        shown_ = shown;
    }

    /// What the view points follow (views/player_view_point.h): the session's players and join counts, and what is
    /// on screen (each column's source as last decided). Player 1's local player: the session's while a seat is in,
    /// else the first the engine lists (the mirror views' columns shape player 1's view with no seat in).
    lss::views::SplitFrame SplitFrameNow(bool splitShown, bool viewsSplit) const noexcept
    {
        lss::views::SplitFrame frame;
        frame.playerOne = session_->PlayerOne() != nullptr ? session_->PlayerOne() : firstPlayer_;
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            const auto at = static_cast<std::size_t>(seat);
            frame.seats[at]       = session_->LocalPlayer(seat);
            frame.joins[at]       = session_->JoinCount(seat);
            frame.onPlayerOne[at] = shown_.OnPlayerOne(seat);
        }
        frame.splitShown  = splitShown;
        frame.viewsSplit  = viewsSplit;
        frame.columns     = columnsListed_;
        return frame;
    }

    /// A key's action: what it did, or why not (the control commands return the same text).
    static void LogAction(const wchar_t* key, const lss::modules::Result& result) noexcept
    {
        Log(L"{}: {}", key, result.text);
    }

    void LogLayoutChange(const lss::players::HeldChange& change) const noexcept
    {
        if (const auto text = lss::players::Describe(layout_, change); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    /// The switch's change (each seat's source change, logged after it by FollowShown, gives the reason).
    static void LogSingleViewChange(lss::players::SingleViewChange change) noexcept
    {
        if (const auto text = lss::players::Describe(change); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    std::optional<lss::session::Session>   session_;
    lss::players::JoinKeys                    keys_;
    ULONGLONG                                 nextStatus_ = 0;
    lss::players::SplitScreenSettings         settings_{{&lss::players::console::VariableInt,
                                                         &lss::players::console::SetVariableInt}};
    lss::players::SplitLayout                 layout_{{&ReadTwoPlayerLayout, &WriteTwoPlayerLayout}};
    lss::players::SingleView                  singleView_{{&ReadSplitSwitch, &WriteSplitSwitch}};
    lss::views::column_source::Override       singleViewChoice_ = lss::views::column_source::Override::Auto;
    lss::views::column_source::Shown          shown_;  // each column's source, as last decided (FollowShown)
    bool                                      spanWas_            = false;  // the window spanned monitors at the last tick
    const void*                               firstPlayer_        = nullptr;  // the first local player listed at the last columns' tick
    int                                       columnsListed_      = 1;  // as the last columns' tick read them
    bool                                      columnsHeld_        = false;  // ... and whether they are held beyond the players
    bool                                      tickSeen_           = false;
    bool                                      rendererInstalled_  = false;
};

}  // namespace

#define LOCALSPLITSCREEN_API __declspec(dllexport)
extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
