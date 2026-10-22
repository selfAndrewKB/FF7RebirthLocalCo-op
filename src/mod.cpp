// LocalSplitScreen: the UE4SS entry point.
//
// This file wires the modules together:
//   1. verify every engine fact the mod relies on (facts/engine.h); if any fails, install nothing;
//   2. if all hold, install the renderer (one view family per player), the session (player 2's join and leave) and
//      its stages, then the module table in order (modules.h), the dev tools and the engine callbacks;
//   3. tick the modules once per engine tick, outside Draw; at unload, remove in reverse.
// Design: Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md

#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "dev/dev_tools.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "hud/hud_placement.h"
#include "hud/minimap.h"
#include "hud/minimap_once.h"
#include "hud/overlay_layout.h"
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
#include "player_two/anim_two_views.h"
#include "player_two/approach_two.h"
#include "player_two/attack_cameras.h"
#include "player_two/attack_framing.h"
#include "player_two/audio_listener_two.h"
#include "player_two/camera_distance.h"
#include "player_two/camera_effects_two.h"
#include "player_two/camera_repair.h"
#include "player_two/charapop_centre.h"
#include "player_two/charapop_near.h"
#include "player_two/chocobo_climb.h"
#include "player_two/chocobo_ride.h"
#include "player_two/climb_context.h"
#include "player_two/combat_rumble.h"
#include "player_two/equal_targeting.h"
#include "player_two/ffb_manager_two.h"
#include "player_two/field_exit.h"
#include "player_two/generation.h"
#include "player_two/leave_watch.h"
#include "player_two/location_rows.h"
#include "player_two/main_camera_shape.h"
#include "player_two/move_type_two.h"
#include "player_two/nav_strand.h"
#include "player_two/near_fade.h"
#include "player_two/pad_rumble.h"
#include "player_two/pad_sticks.h"
#include "player_two/rumble_list_two.h"
#include "player_two/session.h"
#include "player_two/shared_ride.h"
#include "player_two/shop_talk.h"
#include "player_two/shoulder_scale.h"
#include "player_two/synergy_partner.h"
#include "player_two/target_coin.h"
#include "player_two/traversal_camera.h"
#include "player_two/traversal_watch.h"
#include "player_two/trigger_two.h"
#include "player_two/vault_camera.h"
#include "player_two/vending.h"
#include "player_two/walk_limits.h"
#include "player_two/weak_object.h"
#include "player_two/world_actor_reach.h"
#include "players/console_access.h"
#include "players/held_setting.h"
#include "players/join_keys.h"
#include "players/single_view.h"
#include "players/split_layout.h"
#include "players/split_screen_settings.h"
#include "players/split_settings_access.h"
#include "players/tracked_object.h"
#include "players/unreal_access.h"
#include "renderer/additional_family_output.h"
#include "renderer/async_compute.h"
#include "renderer/family_boundary.h"
#include "renderer/me_feedback_once.h"
#include "renderer/residency_off.h"
#include "renderer/submit_hints.h"
#include "renderer/view_family_submission.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"
#include "views/third_view.h"
#include "views/whole_screen.h"
#include "views/window_span.h"

#include <array>
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

constexpr const wchar_t* kVersion = L"0.88.0";  // phase 6 of the 2026-10 cleanup: the splits (moves, not rewrites)

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
        if (session_ && session_->HasPlayerTwo())
        {
            Log(L"unloading with player 2 still in: the per-player rendering and player 2's stages stay installed");
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
            // The control channel alone (no engine redirects), so a script can still quit the game.
            if (const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt)
            {
                session_.emplace(*removeLocalPlayer);
                lss::dev::InstallControl(*session_, layout_, singleView_, false);
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
        lss::modules::Install(*image);  // every other module, in table order (modules.cpp); each logs its own line

        // The dev tools (built with LSS_DEV_TOOLS only): the control channel, the HUD trace and probe, and the watches
        // that only log (dev/dev_tools.h names them); a player's build has none of them.
        lss::dev::Install(*image, *session_, layout_, singleView_, lss::views::ViewPointInstalled());

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
            L"merges the pads. Ctrl+F3 adds a third full-size view (the multi-monitor measurement), Ctrl+F2 spans the "
            L"window across the monitor row");
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
        if (!session_->HasPlayerTwo())
        {
            return;
        }
        lss::views::third_view::Remove(why);  // first: it borrows player 2's join, and its controller is this world's
        const bool left = session_->LeaveNow();
        // Now, not at the next tick: the view points must not keep the old player 2.
        lss::views::Follow(SplitFrameNow(false));
        // The viewport outlives the world: its split switch is cleared while player 1's controller
        // is still this world's.
        LogSingleViewChange(singleView_.Update(false, false));
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
        // The tick's identities (the member's and player 1's pawn's weaks, the driven chocobo) before any module's.
        lss::player_two::generation::BeginTick(lss::player_two::chocobo_ride::DrivenChocobo());
        lss::player_two::pad_rumble::BeforeTick();  // before the world tick: player 2's controller ordered after player 1's
        lss::player_two::combat_rumble::BeforeTick();  // outside every notify: their call sites; the status line
        lss::player_two::rumble_list_two::Tick();      // player 2's list made at join (dropped if player 2 went out another way)
        lss::player_two::equal_targeting::Tick();      // both players' types for the Condition selects (no leader scope held here)
        lss::player_two::target_coin::Tick();          // the blueprint natives' swaps once reflection is up; the summary
        lss::player_two::ffb_manager_two::Tick();      // player 2's component on player 2's own manager, after its BeginPlay
        lss::player_two::audio_listener_two::Tick();  // the controller listener slot; the listeners and the summary (Debug)
        lss::player_two::leave_watch::Tick();          // the last leave's controller, followed until it is freed (logged)
        lss::player_two::vending::Tick();              // a press that opened no shop; player 2 leaving with their shop up
        lss::player_two::shop_talk::Tick();            // the shopkeeper verdicts queued by player 2's pass
        lss::player_two::attack_cameras::Tick();       // the attack-camera status line (Debug)
        lss::player_two::attack_framing::Tick();       // the attack-framing latches' clock and status line (Debug)
        lss::player_two::camera_effects_two::Tick();   // the camera-effect status line (Debug)
        lss::player_two::synergy_partner::Tick();      // humans' synergy partners: session, late give-back, status (Debug)
        if (!tickSeen_)
        {
            tickSeen_ = true;
            Log(L"engine tick callback is live");
        }
        switch (keys_.Poll())
        {
            case lss::players::PlayerIntent::Join: session_->Join(); break;
            case lss::players::PlayerIntent::Leave:
                lss::views::third_view::Remove(L"player 2 leaves (Ctrl+F8)");
                session_->Leave();  // logs its own outcome
                break;
            case lss::players::PlayerIntent::ToggleThirdView:
                if (lss::views::third_view::In())
                {
                    lss::views::third_view::Remove(L"Ctrl+F3");
                }
                else
                {
                    lss::views::third_view::Add();
                }
                break;
            case lss::players::PlayerIntent::ToggleWindowSpan:
                if (lss::views::window_span::In())
                {
                    lss::views::window_span::Leave(L"Ctrl+F2");
                }
                else if (session_->HasPlayerTwo())
                {
                    auto* players = lss::players::unreal::LocalPlayers(lss::players::unreal::GameInstance());
                    lss::views::window_span::Enter(players == nullptr ? 0 : players->Num());
                }
                else
                {
                    Log(L"window span: player 2 is not in (Ctrl+F7 first)");
                }
                break;
            case lss::players::PlayerIntent::ToggleOwnCamera: ToggleOwnCamera(); break;
            case lss::players::PlayerIntent::ToggleLayout: LogLayoutChange(layout_.Toggle()); break;
            case lss::players::PlayerIntent::ToggleFill: ToggleFill(); break;
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
        lss::renderer::PollFamilyBoundary();
        lss::renderer::PollMeFeedbackOnce();
        lss::hud::PollMinimapOnce();
        lss::player_two::location_rows::Poll();
        lss::player_two::location_rows::Tick();
        lss::player_two::field_exit::Watch();
        if (session_->HasPlayerTwo() && lss::player_two::field_exit::FieldLeft())
        {
            LeaveBeforeWorldChange(L"the field stopped running (a field exit the redirected entry did not see)");
        }
        lss::dev::TickBeforeSession();
        // The buggy and the bronco (player_two/shared_ride.h): player 2 stands aside, the game's merge reaches player 1
        // from every pad.
        const bool shared = lss::player_two::shared_ride::Tick();
        lss::player_two::rumble_list_two::CopyPlayerOne(shared && lss::player_two::ffb_manager_two::Attached());
        session_->SetSharedRide(shared);
        lss::player_two::pad_sticks::SetStoodDown(shared);
        lss::player_two::climb_context::Tick();  // this frame's input copies for the climbs on the anim worker (W5)
        session_->Tick();
        lss::dev::TickAfterSession();
        lss::views::third_view::Tick(session_->HasPlayerTwo());  // leaves with player 2; follows player 1's view target
        lss::views::window_span::Tick(session_->HasPlayerTwo());  // leaves with player 2; holds the window's rect
        lss::player_two::walk_limits::Tick();  // after the session's tick: a take's copy is overwritten this frame

        // Player 1's single view for the game's whole-screen states (players/single_view.h), and while
        // player 2 waits for a member (only player 1 plays then; the user's decision, 2026-10-01): while
        // it holds, player 2 stays in but the split's presentation is off, as with one player.
        const bool playerTwoIn = session_->HasPlayerTwo();
        const bool autoMode    = singleView_.Mode() == lss::players::SingleViewMode::Auto;
        const bool waits       = PlayerTwoWaits();
        LogSingleViewChange(singleView_.Update(
            playerTwoIn,
            lss::players::SingleViewWanted(autoMode, WholeScreenShown(playerTwoIn), waits, session_->StandsAside())));
        const bool splitShown = playerTwoIn && !singleView_.Single();
        lss::player_two::nav_strand::Tick(splitShown);  // a stranded player 2 is warped back to the party
        lss::player_two::camera_repair::Tick();
        lss::player_two::traversal_watch::Tick();
        lss::player_two::trigger_two::Tick();
        lss::player_two::traversal_camera::Tick();  // player 2's idle camera holders; the collision watch (dev)
        lss::player_two::near_fade::Tick();  // player 2's character's fade state (Debug, on change)
        lss::player_two::approach_two::Tick();  // the crossing walk-up calls, swapped once from here
        lss::player_two::move_type_two::Tick();  // player 2's deferred walk-cap clear, the layer-2 watch
        lss::player_two::vault_camera::Tick();  // player 2's vault cameras end on player 2's camera (lever L2)
        lss::player_two::chocobo_climb::Tick();  // player 2's climb camera's states and the summary (Debug)
        lss::player_two::camera_distance::TickFraming(playerTwoIn);  // per-layout distance steps (roadmap 2b)
        lss::player_two::shoulder_scale::Poll(splitShown);
        lss::player_two::anim_two_views::Tick(splitShown);  // its frame count, before the world tick
        lss::player_two::charapop_near::Tick(splitShown, session_->PlaysMember());
        lss::player_two::charapop_centre::Tick(splitShown, session_->PlaysMember());
        lss::player_two::world_actor_reach::Tick(session_->PlaysMember());
        lss::dev::TickWatches(splitShown);  // the dev-only watches, after the levers they observe (empty in a player's build)
        lss::hud::minimap::Tick(splitShown);  // outside the overlay render, before it
        if (lss::views::WholeScreenInstalled())
        {
            lss::views::UpdateStreamViews(splitShown);  // needs the game-state check whole_screen verified
        }
        // Filled halves: each main camera's own 16:9 constraint, cleared once by the game's setter. FillsHalves is
        // false without the view-point entry (nothing shapes the halves then), so the cameras keep their constraint.
        lss::player_two::main_camera_shape::Update(lss::player_two::generation::Active() && splitShown &&
                                                   lss::views::FillsHalves());
        // The timing method before the gap recorder goes off, and back after it returns (SettingsOrder).
        for (const auto step : lss::players::SettingsOrder(playerTwoIn))
        {
            auto& setting = settings_[static_cast<std::size_t>(step)];
            LogHeld(setting.Update(playerTwoIn, lss::players::SettingOf(step).value), L"split screen",
                    setting.Name());
        }
        if (playerTwoIn != submitHintsSkipped_)
        {
            submitHintsSkipped_ = playerTwoIn;
            lss::renderer::SetSubmitHintSkip(playerTwoIn);
        }
        LogLayoutChange(layout_.Update(playerTwoIn));
        lss::views::Follow(SplitFrameNow(splitShown));  // after the layout: the halves' shape reads it
        lss::hud::SetSplitActive(splitShown);
        lss::hud::UpdateHudPlacement(splitShown && lss::hud::BandEnabled());

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

    /// What the view points follow (views/player_view_point.h): the session's players and join count, and whether
    /// the split is on screen.
    lss::views::SplitFrame SplitFrameNow(bool splitShown) const noexcept
    {
        return {session_->PlayerOne(), session_->PlayerTwo(), session_->JoinCount(), splitShown};
    }

    void ToggleFill() noexcept
    {
        if (!lss::views::ViewPointInstalled())
        {
            Log(L"filled halves unavailable (the view-point entry is not installed)");
            return;
        }
        if (lss::views::ToggleFill())
        {
            Log(L"split halves fill the screen (no bars)");
        }
        else
        {
            Log(L"split halves kept 16:9 with bars (the game's own shape)");
        }
    }

    void LogLayoutChange(const lss::players::HeldChange& change) const noexcept
    {
        if (const auto text = lss::players::Describe(layout_, change); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    void ToggleOwnCamera() noexcept
    {
        if (!lss::views::ViewPointInstalled())
        {
            Log(L"camera choice unavailable (the view-point entry is not installed)");
            return;
        }
        if (!session_->HasPlayerTwo())
        {
            Log(L"camera choice: no player 2 (Ctrl+F7 adds one)");
            return;
        }
        if (lss::views::ToggleOwnCamera())
        {
            Log(L"player 2's half shows player 2's own camera");
        }
        else
        {
            Log(L"player 2's half shows player 1's camera");
        }
    }

    /// Whether the game shows something that takes the whole screen (a full-screen menu, the map
    /// screen, a cutscene), from the game's own state (views/whole_screen.h). Read only while player 2
    /// is in and the automatic choice is on; each change is logged there.
    bool WholeScreenShown(bool playerTwoIn) const noexcept
    {
        return playerTwoIn && lss::views::WholeScreenInstalled() &&
               singleView_.Mode() == lss::players::SingleViewMode::Auto &&
               lss::views::ReasonOf(lss::views::ReadScreenSignals()) != lss::views::WholeScreenReason::None;
    }

    /// Player 2 is in without a member, waiting on the 2nd slot's (player_two/session.h), with the
    /// automatic choice on. Each change is logged.
    bool PlayerTwoWaits() noexcept
    {
        const bool waits = session_->WaitsForMember() && singleView_.Mode() == lss::players::SingleViewMode::Auto;
        if (waits != playerTwoWaited_)
        {
            playerTwoWaited_ = waits;
            Log(L"{}", waits ? L"player 2 waits for a party member: player 1's single view until player 2 takes one"
                             : L"player 2 no longer waits for a party member");
        }
        return waits;
    }

    static void LogSingleViewChange(const lss::players::SingleViewChange& change) noexcept
    {
        if (const auto text = lss::players::Describe(change); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    static void LogHeld(const lss::players::HeldChange& change, const wchar_t* label, const wchar_t* name) noexcept
    {
        if (const auto text = lss::players::Describe(change, label, name); !text.empty())
        {
            Log(L"{}", text);
        }
    }

    /// One held console setting, for the settings_ array (split_screen_settings.h, by SettingsStep).
    static lss::players::HeldSetting<std::int32_t> ConsoleSettingOf(lss::players::SettingsStep step) noexcept
    {
        return {{&lss::players::console::VariableInt, &lss::players::console::SetVariableInt},
                lss::players::SettingOf(step).name};
    }

    std::optional<lss::player_two::Session>   session_;
    lss::players::JoinKeys                    keys_;
    ULONGLONG                                 nextStatus_ = 0;
    std::array<lss::players::HeldSetting<std::int32_t>, 3> settings_{
        ConsoleSettingOf(lss::players::SettingsStep::Resolution), ConsoleSettingOf(lss::players::SettingsStep::Timing),
        ConsoleSettingOf(lss::players::SettingsStep::Gap)};
    bool                                      submitHintsSkipped_ = false;  // the join/leave switch's last state
    lss::players::SplitLayout                 layout_{{&ReadTwoPlayerLayout, &WriteTwoPlayerLayout}};
    lss::players::SingleView                  singleView_{{&ReadSplitSwitch, &WriteSplitSwitch}};
    bool                                      playerTwoWaited_ = false;  // as last logged
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
