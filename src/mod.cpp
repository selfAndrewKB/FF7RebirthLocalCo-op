// LocalSplitScreen: the UE4SS entry point.
//
// This file wires the modules together:
//   1. verify every engine fact the mod relies on (facts/engine.h); if any fails, install nothing;
//   2. if all hold, install the renderer (one view family per player), the session (player 2's join and leave) and
//      its stages, then the module table in order (modules.h), the dev tools and the engine callbacks;
//   3. tick the modules once per engine tick, outside Draw, in two phases around the session's tick (modules.h);
//      at unload, remove in reverse.
// Design: Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md

#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "dev/dev_tools.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "hud/layout/hud_placement.h"
#include "hud/layout/overlay_layout.h"
#include "modules.h"
#include "platform/code_memory.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/level_names.h"
#include "platform/log.h"
#include "platform/memory_watch.h"
#include "platform/module_folder.h"
#include "platform/thread_times.h"
#include "platform/verified_code.h"
#include "player_two/feedback/ffb_manager_two.h"
#include "player_two/feedback/rumble_list_two.h"
#include "player_two/generation.h"
#include "player_two/input/climb_context.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/rides/shared_ride.h"
#include "player_two/session/field_exit.h"
#include "player_two/session/session.h"
#include "player_two/weak_object.h"
#include "player_two/world/charapop_centre.h"
#include "player_two/world/charapop_near.h"
#include "player_two/world/world_actor_reach.h"
#include "players/console_access.h"
#include "players/join_keys.h"
#include "players/seats.h"
#include "players/single_view.h"
#include "players/split_layout.h"
#include "players/split_screen_settings.h"
#include "players/split_settings_access.h"
#include "players/tracked_object.h"
#include "renderer/additional_family_output.h"
#include "renderer/async_compute.h"
#include "renderer/family_boundary.h"
#include "renderer/residency_off.h"
#include "renderer/view_family_submission.h"
#include "views/player_view_point.h"
#include "views/display_align.h"
#include "views/mirror_player.h"
#include "views/whole_screen.h"
#include "views/window_span.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

namespace Hook = RC::Unreal::Hook;
using lss::platform::Log;

constexpr const wchar_t* kVersion = L"0.93.0";  // seats step 4: each player's own HUD pass (plan/seats.md)

/// How often player 2's status line is written while player 2 is in.
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
    auto* controller = static_cast<RC::Unreal::UObject*>(lss::player_two::generation::PlayerOneController());
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

// The three-player layout, held at three equal columns while three local players are listed (views/columns.h).
constexpr std::uint8_t   kThreeColumns     = 2;  // EThreePlayerSplitScreenType::Vertical
constexpr const wchar_t* kThreeLayoutName  = L"ThreePlayerSplitscreenLayout";

std::optional<std::uint8_t> ReadThreePlayerLayout(const wchar_t*) noexcept
{
    return lss::players::split_settings::ThreePlayerLayout();
}

bool WriteThreePlayerLayout(const wchar_t*, std::uint8_t value) noexcept
{
    return lss::players::split_settings::SetThreePlayerLayout(value);
}

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
        for (const auto id : {tickCallback_, loadMapCallback_})
        {
            if (id != Hook::ERROR_ID)
            {
                Hook::UnregisterCallback(id);
            }
        }

        // With player 2 still in, the engine keeps rendering two views and calling player 2's
        // wrappers. Putting the engine's own submission back would render them in one family, the
        // configuration this mod exists to avoid, and player 2's camera manager depends on its
        // wrappers. Removing player 2 here is not an option: the destructor may run off the game
        // thread. So everything stays installed; the DLL is pinned, so it stays valid. The characters
        // raised for player 2's view are given back either way (that setter runs from any thread), and the NPC
        // centre when this runs on the game thread.
        lss::player_two::charapop_near::Release();
        lss::player_two::charapop_centre::Release();
        lss::player_two::world_actor_reach::Release();
        // The monitors the span lined up go back for the Windows session (plain Win32, any thread); the window itself
        // is the engine's, reshaped back only on the game thread.
        if (lss::player_two::generation::OnGameThread())
        {
            lss::views::window_span::Leave(L"the mod unloads");
        }
        lss::views::display_align::Restore();
        if (session_ && (session_->HasPlayerTwo() || lss::views::mirror_player::Count() > 0))
        {
            Log(L"unloading with player 2 or a mirror view still in: the per-player rendering and the stages stay "
                L"installed");
            return;
        }
        // The reverse of the install order: the field-exit catch and the dev tools (installed after the table),
        // the table (modules.cpp walks it backwards), the session's stages, the renderer.
        lss::player_two::field_exit::Remove();
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
                    lss::players::console::RegisterCommands([this] { return session_->HasPlayerTwo(); });
                }
                catch (...)
                {
                    Log(L"control: registering the console's commands failed (out of memory?)");
                }
#endif
                lss::dev::InstallControl(*session_);
            }
            const Hook::FCallbackOptions watchOptions{false, false, STR("LocalSplitScreen"), STR("frame watch")};
            tickCallback_ = Hook::RegisterEngineTickPreCallback(
                [](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) {
                    lss::platform::frame_watch::OnFrameStart();
                    lss::dev::TickBeforeSession();
                },
                watchOptions);
            return;
        }

        if (image && image->Build() != nullptr)
        {
            Log(L"game version {}{}", image->Build()->name,
                image->Build()->research ? L" (the research build)" : L" (addresses translated from the research build)");
        }
        const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt;
        if (removeLocalPlayer)
        {
            // The game's end-of-frame state reset between two families (the main-menu crash's fix),
            // before the submission that asks for it; logs its own outcome.
            lss::renderer::InstallFamilyBoundary(*image);
        }
        if (!removeLocalPlayer || !InstallRenderer(*image))
        {
            Log(L"standing aside: nothing installed, the game runs unmodified");
            return;
        }
        lss::player_two::weak_object::Install(*image);    // the engine's weak pointers, once, for every module
        lss::facts::InstallInBattle(*image);  // the game's in-battle test, once, for the stages that ask it
        session_.emplace(*removeLocalPlayer);
        session_->InstallStages(*image);
        // A seat's join takes the mirror views out right before its CreatePlayer (views/mirror_player.h).
        session_->SetClearMirrors([]() noexcept { return lss::views::mirror_player::Clear(L"a player joins"); });
        lss::modules::Install(*image);  // every other module, in table order (modules.cpp); each logs its own line

        // The dev tools (built with LSS_DEV_TOOLS only): the console, the HUD trace and probe, and the watches
        // that only log (dev/dev_tools.h names them); a player's build has none of them. Every control command is
        // registered first, each owner's (modules.cpp lists them), so the console opens on the whole table.
#if LSS_DEV
        lss::modules::RegisterCommands(*session_, layout_, singleView_);
#endif
        lss::dev::Install(*image, *session_);

        const Hook::FCallbackOptions tickOptions{false, false, STR("LocalSplitScreen"), STR("player intent")};
        tickCallback_ = Hook::RegisterEngineTickPreCallback(
            [this](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { OnEngineTick(); },
            tickOptions);

        // Player 2 leaves before any map load: the new world would log player 2 in again, and its
        // camera manager's fixed-name spawns would collide with player 1's outside a join.
        const Hook::FCallbackOptions loadOptions{false, false, STR("LocalSplitScreen"), STR("leave before map load")};
        loadMapCallback_ = Hook::RegisterLoadMapPreCallback(
            [this](auto&, auto&&...) { LeaveBeforeWorldChange(L"a map is loading"); }, loadOptions);
        // The same before a retry, reload or load leaves the field inside this world (no map load).
        if (!lss::player_two::field_exit::Install(
                *image,
                [](void* self) { static_cast<LocalSplitScreenMod*>(self)->LeaveBeforeWorldChange(L"the game leaves the field"); },
                this))
        {
            Log(L"a retry, reload or load is caught only by the engine tick's check of the field running byte");
        }

        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2, Ctrl+F8 removes player 2, Ctrl+F5 switches "
            L"top/bottom and side by side, Ctrl+F4 switches filled halves and 16:9 halves with bars. Player 2 starts on "
            L"their own camera and pad; for testing, Ctrl+F6 switches their half to player 1's camera and Ctrl+F9 "
            L"merges the pads. Ctrl+F2 spans the window across the monitor row, one monitor per player; Ctrl+F3 "
            L"switches the three-player monitor mode (three monitors, player 1's picture in the empty columns)");
        const auto pool = lss::platform::CodePool();
        Log(L"code pool: {} piece(s), {} bytes of placed code in {} block(s) of 64 KiB (one search each)", pool.pieces,
            pool.bytes, pool.blocks);
    }

private:
    /// The two renderer changes, both or neither. The output rule goes first: with it alone, one
    /// player renders exactly as before (the game's single family is never marked additional), so
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

    /// Player 2 leaves before the world changes under it (a map load, or the game leaving the field
    /// for a retry, reload or load); the session joins again once the new field is ready.
    void LeaveBeforeWorldChange(const wchar_t* why) noexcept
    {
        // The mirror views first, with or without player 2: their controllers are this world's, and a map load would
        // spawn them again outside a join. They come back once a join could run.
        lss::modules::ClearMirrorsForWorldChange(why);
        if (!session_->HasPlayerTwo())
        {
            return;
        }
        const bool left = session_->LeaveNow();
        // Now, not at the next tick: the view points must not keep the old player 2.
        lss::views::Follow(SplitFrameNow(false, false, false));
        // The viewport outlives the world: its split switch is cleared while player 1's controller
        // is still this world's.
        LogSingleViewChange(singleView_.Update(false, {}));
        if (left)
        {
            Log(L"{}: player 2 left (and joins again once the field is ready)", why);
        }
        else
        {
            Log(L"WARNING: {} and player 2 could not be removed; a new world would log player 2 in, and its camera "
                L"manager's spawns are expected to collide with player 1's",
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
            case lss::players::PlayerIntent::Join: session_->Join(lss::players::kPlayerTwoSeat); break;
            case lss::players::PlayerIntent::Leave:
                lss::modules::LeavePlayerTwo(*session_, L"player 2 leaves (Ctrl+F8)");  // logs its own outcome
                break;
            case lss::players::PlayerIntent::ToggleMonitorMode:
                LogAction(L"three-player monitor mode (Ctrl+F3)",
                          lss::modules::SetMonitorMode(!lss::views::window_span::Held()));
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
                if (lss::player_two::charapop_near::Installed())
                {
                    lss::player_two::charapop_near::SetDetail(
                        lss::player_two::charapop_near::NextDetail(lss::player_two::charapop_near::CurrentDetail()));
                }
                break;
            case lss::players::PlayerIntent::ToggleNpcCentre:
                if (lss::player_two::charapop_centre::Installed())
                {
                    lss::player_two::charapop_centre::SetEnabled(!lss::player_two::charapop_centre::Enabled());
                }
                break;
            case lss::players::PlayerIntent::None: break;
        }
        lss::player_two::field_exit::Watch();
        if ((session_->HasPlayerTwo() || lss::views::mirror_player::Count() > 0) &&
            lss::player_two::field_exit::FieldLeft())
        {
            LeaveBeforeWorldChange(L"the field stopped running (a field exit the redirected entry did not see)");
        }
        lss::dev::TickBeforeSession();
        // The buggy and the bronco (player_two/rides/shared_ride.h): player 2 stands aside, the game's merge reaches
        // player 1 from every pad.
        const bool shared = lss::player_two::shared_ride::Tick();
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)  // each seat with its own force-feedback manager
        {
            lss::player_two::rumble_list_two::CopyPlayerOne(seat,
                                                            shared && lss::player_two::ffb_manager_two::Attached(seat));
        }
        session_->SetSharedRide(shared);
        lss::player_two::pad_sticks::SetStoodDown(shared);
        lss::player_two::climb_context::Tick();  // this frame's input copies for the climbs on the anim worker (W5)
        session_->Tick();
        // The columns right after the session: the mirror views toward them, the window span to them.
        bool rejoining = false;
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            rejoining = rejoining || session_->RejoinPending(seat);
        }
        const lss::modules::Columns columns = lss::modules::TickColumns(rejoining);
        columnsListed_                      = columns.map.count;
        lss::dev::TickAfterSession();
        // Player 1's single view for the game's whole-screen states (players/single_view.h), and while
        // player 2 waits for a member (only player 1 plays then; the user's decision, 2026-10-01): while
        // it holds, player 2 stays in but the split's presentation is off, as with one player. While columns are
        // held (the three-player monitor mode), the same moments keep the columns instead (the columns rule: a held
        // column is never black): player 2's view shows player 1's camera and HUD, as a mirror view does.
        const bool playerTwoIn = session_->HasPlayerTwo();
        const auto mode        = singleView_.Mode();
        const bool autoMode    = mode == lss::players::SingleViewMode::Auto;
        const lss::players::SingleViewInputs inputs{
            playerTwoIn && autoMode && lss::views::WholeScreenShown(),  // read only when it can matter
            session_->WaitsForMember(), session_->StandsAside()};
        LogSingleViewChange(singleView_.Update(playerTwoIn && !columns.held, inputs));
        const bool onPlayerOne = playerTwoIn && columns.held &&
                                 (mode == lss::players::SingleViewMode::Single ||
                                  lss::players::SingleViewWanted(autoMode, inputs.wholeScreenShown,
                                                                 inputs.waitsForMember, inputs.standsAside));
        if (onPlayerOne != onPlayerOne_)
        {
            onPlayerOne_ = onPlayerOne;
            Log(L"single view: {}", onPlayerOne ? L"the columns stay (columns held): player 2's view shows player 1's "
                                                  L"camera and HUD"
                                                : L"player 2's view is player 2's own again");
        }
        const bool splitShown = playerTwoIn && !singleView_.Single() && !onPlayerOne;
        const bool viewsSplit = columns.map.count > 1 && !singleView_.Single();
        lss::modules::Frame frame;
        frame.splitShown = splitShown;
        frame.columns    = columns.map.count;
        frame.onPlayerOne = onPlayerOne;
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            frame.seats[static_cast<std::size_t>(seat)].state = session_->State(seat);
        }
        lss::modules::TickAfterSession(frame);
        lss::dev::TickWatches(splitShown);  // the dev watches, after the levers they observe
        // The settings several views need, while more than one view family renders (player 2, or a mirror view).
        settings_.Update(columns.map.count > 1, [](const std::wstring& line) { Log(L"{}", line); });
        // The layout by column count: the player's two-player choice with two local players (player 2, or a mirror
        // beside player 1), the three equal columns with three.
        LogLayoutChange(layout_.Update(columns.map.count == 2));
        if (const auto text = lss::players::Describe(threeLayout_.Update(columns.map.count == 3, kThreeColumns),
                                                      L"three columns", kThreeLayoutName);
            !text.empty())
        {
            Log(L"{}", text);
        }
        // After the layout: the halves' shape reads it; the one place "the split is shown" is kept (views::SplitShown,
        // views::ViewsSplit).
        lss::views::Follow(SplitFrameNow(splitShown, onPlayerOne, viewsSplit));
        lss::hud::UpdateHudPlacement(viewsSplit && lss::hud::BandEnabled());

        const ULONGLONG now = GetTickCount64();
        if (!session_->HasPlayerTwo() || !lss::platform::frame_watch::Detailed())  // perf-log: the periodic status
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

    /// What the view points follow (views/player_view_point.h): the session's players and join counts, and what is
    /// on screen.
    lss::views::SplitFrame SplitFrameNow(bool splitShown, bool onPlayerOne, bool viewsSplit) const noexcept
    {
        lss::views::SplitFrame frame;
        frame.playerOne = session_->PlayerOne();
        for (int seat = 1; seat <= lss::players::kSeatsBuilt; ++seat)
        {
            frame.seats[static_cast<std::size_t>(seat)] = session_->LocalPlayer(seat);
            frame.joins[static_cast<std::size_t>(seat)] = session_->JoinCount(seat);
        }
        frame.splitShown  = splitShown;
        frame.onPlayerOne = onPlayerOne;
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

    /// The waits edge first (it explains the single view that follows), then the switch's change.
    static void LogSingleViewChange(const lss::players::SingleViewChange& change) noexcept
    {
        if (const auto text = lss::players::DescribeWaits(change); !text.empty())
        {
            Log(L"{}", text);
        }
        if (const auto text = lss::players::Describe(change); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    std::optional<lss::player_two::Session>   session_;
    lss::players::JoinKeys                    keys_;
    ULONGLONG                                 nextStatus_ = 0;
    lss::players::SplitScreenSettings         settings_{{&lss::players::console::VariableInt,
                                                         &lss::players::console::SetVariableInt}};
    lss::players::SplitLayout                 layout_{{&ReadTwoPlayerLayout, &WriteTwoPlayerLayout}};
    lss::players::SingleView                  singleView_{{&ReadSplitSwitch, &WriteSplitSwitch}};
    lss::players::HeldSetting<std::uint8_t>   threeLayout_{{&ReadThreePlayerLayout, &WriteThreePlayerLayout},
                                                           kThreeLayoutName};
    int                                       columnsListed_      = 1;  // as the last columns' tick read them
    bool                                      onPlayerOne_        = false;  // player 2's view on player 1's (columns held)
    bool                                      tickSeen_           = false;
    bool                                      rendererInstalled_  = false;
    Hook::GlobalCallbackId                    tickCallback_       = Hook::ERROR_ID;
    Hook::GlobalCallbackId                    loadMapCallback_    = Hook::ERROR_ID;
};

}  // namespace

#define LOCALSPLITSCREEN_API __declspec(dllexport)
extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
