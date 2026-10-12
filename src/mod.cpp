// LocalSplitScreen: the UE4SS entry point.
//
// This file wires the modules together and holds no logic of its own:
//   1. verify every engine fact the mod relies on (platform/engine_addresses.h);
//   2. if all hold, install the view-family submission (renderer/) and start listening for
//      player intent on the game thread (players/); if any fails, install nothing.
// Design: Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md

#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "dev/dev_tools.h"
#include "hud/action_icon_base.h"
#include "hud/hud_placement.h"
#include "hud/layout_scale.h"
#include "hud/minimap_once.h"
#include "hud/navimap_watch.h"
#include "hud/minimap.h"
#include "hud/overlay_layout.h"
#include "hud/player_two_pass.h"
#include "hud/area_icons_two.h"
#include "hud/target_views.h"
#include "hud/prompt_rows.h"
#include "hud/field_markers.h"
#include "hud/root_overlay_render.h"
#include "hud/second_image.h"
#include "hud/user_widgets.h"
#include "platform/code_memory.h"
#include "platform/engine_addresses.h"
#include "platform/frame_watch.h"
#include "platform/memory_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "player_two/field_exit.h"
#include "player_two/generation.h"
#include "player_two/main_camera_shape.h"
#include "player_two/camera_distance.h"
#include "player_two/camera_repair.h"
#include "player_two/framing_watch.h"
#include "player_two/location_rows.h"
#include "player_two/shoulder_scale.h"
#include "player_two/walk_limits.h"
#include "player_two/handover_watch.h"
#include "player_two/field_pickup.h"
#include "player_two/rumble_watch.h"
#include "player_two/pad_rumble.h"
#include "player_two/combat_rumble.h"
#include "player_two/rumble_list_two.h"
#include "player_two/audio_listener_two.h"
#include "player_two/chest_watch.h"
#include "player_two/vending.h"
#include "player_two/interact_watch.h"
#include "player_two/charapop_watch.h"
#include "player_two/chocobo_climb.h"
#include "player_two/climb_context.h"
#include "player_two/approach_two.h"
#include "player_two/move_type_two.h"
#include "player_two/near_fade.h"
#include "player_two/anim_two_views.h"
#include "player_two/charapop_near.h"
#include "player_two/charapop_centre.h"
#include "player_two/world_actor_reach.h"
#include "player_two/ride_probe.h"
#include "player_two/ride_watch.h"
#include "player_two/shared_ride.h"
#include "player_two/wheelie_ride.h"
#include "player_two/wheelie_watch.h"
#include "player_two/pad_sticks.h"
#include "player_two/chocobo_ride.h"
#include "player_two/nav_strand.h"
#include "player_two/ride_framing_watch.h"
#include "player_two/traversal_camera.h"
#include "player_two/traversal_watch.h"
#include "player_two/trigger_two.h"
#include "player_two/vault_camera.h"
#include "player_two/party_warp.h"
#include "player_two/session.h"
#include "players/join_keys.h"
#include "players/single_view.h"
#include "players/split_layout.h"
#include "players/split_screen_settings.h"
#include "players/tracked_object.h"
#include "players/unreal_access.h"
#include "renderer/additional_family_output.h"
#include "renderer/async_compute.h"
#include "renderer/family_boundary.h"
#include "renderer/me_feedback_once.h"
#include "renderer/submission_census.h"
#include "renderer/submit_hints.h"
#include "renderer/residency_off.h"
#include "renderer/view_family_submission.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"
#include "views/view_shape.h"
#include "views/whole_screen.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <system_error>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

namespace Hook = RC::Unreal::Hook;
using lss::platform::Log;

constexpr const wchar_t* kVersion = L"0.64.0";

/// How often player 2's status line is written while player 2 is in.
constexpr ULONGLONG kStatusIntervalMs = 10'000;

/// How often, in engine ticks, the split halves' shape is read while player 2 is in.
constexpr std::uint32_t kHalfShapeIntervalTicks = 30;

/// Checks every engine fact against the running game and logs each one that fails.
/// Returns the live address of RemoveLocalPlayer when all hold.
std::optional<std::uintptr_t> VerifyEngine(const lss::platform::GameImage& image) noexcept
{
    bool allHold = true;
    for (const auto& fact : lss::engine::kVerifiedCode)
    {
        if (!image.BytesMatch(fact.idaAddress, fact.bytes))
        {
            Log(L"check failed: {} (IDA 0x{:X}) does not have the expected bytes", fact.what, fact.idaAddress);
            allHold = false;
        }
    }

    const auto entryHolds = image.ReadPointer(lss::engine::kBeginRenderingViewFamilyEntry);
    const auto engineFn   = image.Rebase(lss::engine::kBeginRenderingViewFamily);
    if (!entryHolds || !engineFn || *entryHolds != *engineFn)
    {
        Log(L"check failed: the renderer's vtable entry does not hold BeginRenderingViewFamily "
            L"(a game update, or another mod already replaced it)");
        allHold = false;
    }

    // The GetViewPoint entry is not checked here: the split screen works without it (with bars),
    // and its install is itself a compare-and-swap against the expected function
    // (views/player_view_point).

    const auto removeLocalPlayer = image.Rebase(lss::engine::kRemoveLocalPlayer);
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
    return lss::players::unreal::SplitscreenForceDisabled(PlayerOneWorldContext());
}

bool WriteSplitSwitch(bool disabled) noexcept
{
    return lss::players::unreal::SetForceDisableSplitscreen(PlayerOneWorldContext(), disabled);
}

class LocalSplitScreenMod final : public RC::CppUserModBase
{
public:
    LocalSplitScreenMod()
    {
        ModName        = STR("LocalSplitScreen");
        ModVersion     = STR("0.64.0");
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
        if (session_)
        {
            session_->RemoveStages();
        }
        if (viewPointInstalled_)
        {
            lss::views::RemovePlayerViewPoint();
        }
        lss::hud::RemoveRootOverlayRender();
        lss::player_two::field_exit::Remove();
        lss::player_two::nav_strand::Remove();
        lss::player_two::ride_framing_watch::Remove();
        lss::player_two::trigger_two::Remove();
        lss::player_two::chocobo_climb::Remove();
        lss::player_two::traversal_camera::Remove();
        lss::player_two::climb_context::Remove();
        lss::player_two::near_fade::Remove();
        lss::player_two::approach_two::Remove();
        lss::player_two::move_type_two::Remove();
        lss::player_two::anim_two_views::Remove();
        lss::player_two::rumble_list_two::Remove();
        lss::player_two::audio_listener_two::Remove();
        lss::player_two::traversal_watch::Remove();
        lss::player_two::vault_camera::Remove();
        lss::hud::target_views::Remove();
        lss::hud::area_icons_two::Remove();
        lss::hud::user_widgets::Remove();
        lss::hud::minimap::Remove();
        lss::hud::RemoveMinimapOnce();
        lss::player_two::location_rows::Remove();
        lss::player_two::shoulder_scale::Remove();
        lss::hud::RemoveSecondImage();
        lss::hud::RemovePlayerTwoPass();
        lss::dev::Remove();
        lss::hud::RemoveLayoutScale();
        lss::hud::RemoveOverlayLayout();
        lss::renderer::RemoveMeFeedbackOnce();
        lss::renderer::RemoveSubmitHintSkip();
        lss::renderer::RemoveSubmissionCensus();
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
            constexpr std::pair<const wchar_t*, lss::platform::Level> kLevels[] = {
                {L"error", lss::platform::Level::Error}, {L"warning", lss::platform::Level::Warning},
                {L"info", lss::platform::Level::Info},   {L"debug", lss::platform::Level::Debug},
                {L"trace", lss::platform::Level::Trace}};
            for (const auto& [name, value] : kLevels)
            {
                if (word == name)
                {
                    lss::platform::SetLevel(value);
                    Log(L"log level {} (log-level.txt)", name);
                }
            }
        }
        // Recognises the game build (and so every PerBuild value) before any thread of ours can read one; the later
        // Locate calls find the same build.
        static_cast<void>(lss::platform::GameImage::Locate());
        lss::renderer::ReportResidency();
        lss::renderer::ReportAsyncCompute();
        lss::platform::memory_watch::Start();  // its own thread, from launch to exit

        // A measuring control: with `watch_only.txt` in the mod's folder, nothing is installed but the
        // frame-hitch watch, so the game's own frame times can be compared with the mod's.
        std::error_code missing;
        // `perf_log.txt` turns the perf log on from launch (the frame-time and per-thread CPU lines every 10 s), for
        // machines without the control scripts.
        if (std::filesystem::exists(lss::platform::ModFolder() / L"perf_log.txt", missing))
        {
            lss::platform::frame_watch::SetDetailed(true);
            Log(L"perf log on (perf_log.txt)");
        }
        if (std::filesystem::exists(lss::platform::ModFolder() / L"watch_only.txt", missing))
        {
            Log(L"watch only (watch_only.txt): nothing installed except the frame-hitch watch; the game runs unmodified");
            lss::platform::frame_watch::SetDetailed(true);  // the hitch summary is this control's whole output
            // The control channel alone (no engine redirects), so a script can still quit the game.
            const auto image = lss::platform::GameImage::Locate();
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

        const auto image = lss::platform::GameImage::Locate();
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
        session_.emplace(*removeLocalPlayer);
        session_->InstallStages(*image);
        lss::renderer::InstallSubmitHintSkip();  // parallel passes without their own GPU submission (player 2 in)
        lss::renderer::InstallSubmissionCensusIfAsked();  // diagnosis: census.txt
        lss::renderer::InstallMeFeedbackOnce(*image);  // one scenery-streaming hand-in per frame, player 1's family's

        // The view-point entry serves the filled halves and the choice of player 2's camera. The
        // split screen runs without it (halves kept 16:9 with bars, player 2 on its own camera);
        // those keys then report that they are unavailable.
        viewPointInstalled_ = lss::views::InstallPlayerViewPoint(*image);
        if (!viewPointInstalled_)
        {
            Log(L"ULocalPlayer::GetViewPoint's entry does not hold the engine's function; filled halves and the "
                L"camera choice are unavailable");
        }
        mainCameraShape_ = viewPointInstalled_ && lss::player_two::main_camera_shape::Install(*image);
        wholeScreen_     = lss::views::InstallWholeScreen(*image);  // menus, the map, cutscenes: player 1's single view
        lss::views::InstallStreamViews(*image, viewPointInstalled_);  // each player's camera streams scenery detail
        lss::player_two::party_warp::Install(*image);  // the game's follow warp, for a stranded player 2
        lss::player_two::nav_strand::Install(*image);  // player 2's member stranded off the loaded navmesh
        lss::player_two::camera_repair::Install(*image);  // a non-finite pose on player 2's camera: the game's camera reset
        lss::player_two::ride_framing_watch::Install(*image);  // both cameras' ride framing and who changes it (logged)
        lss::player_two::traversal_watch::Install(*image);  // ladders, climb spots, glide and jet for player 2 (logged)
        lss::player_two::climb_context::Install(*image);  // climbs on the anim worker read their own climber's input (W5); before traversal_camera, whose holders need its Ticks
        lss::player_two::traversal_camera::Install(*image);  // player 2's traversal gets the game's traversal cameras
        lss::player_two::near_fade::Install(*image);  // player 1's camera never fades player 2's character out
        lss::player_two::approach_two::Install(*image);  // walk-up traversals read player 2's stick in player 2's camera basis
        lss::player_two::move_type_two::Install(*image);  // player 2's walk-step cap ends on player 2's controller
        lss::player_two::anim_two_views::Install(*image);  // characters near player 2 animate by the nearer camera
        lss::player_two::trigger_two::Install(*image);  // player 2's trigger read after player 1's (events held back, logged)
        lss::player_two::vault_camera::Install(*image);  // player 2's chocobo vaults without taking both ride framings
        lss::player_two::chocobo_climb::Install(*image);  // player 2's chocobo climbs walls, with its own climb camera

        // The HUD's layout in split views (hud/overlay_layout.h). Optional like the view point: without
        // it the HUD keeps the game's full-screen layout, cropped to each half.
        if (lss::hud::InstallOverlayLayout(*image))
        {
            lss::hud::InstallLayoutScale(*image);  // the contain rule; unchanged for 16:9
        }
        lss::hud::user_widgets::Install();          // HUD widgets as the engine constructs them
        lss::hud::InstallSecondImage(*image);        // player 2's HUD image, on (the hud-image command switches it)
        lss::hud::InstallMinimapOnce(*image);        // the minimap's map rendered by player 1's pass only
        lss::hud::navimap_watch::Install(*image);    // the minimap's objects, logged (read-only)
        lss::hud::minimap::Install(*image);      // player 2's minimap in player 2's half
        lss::player_two::framing_watch::Install(*image);  // each player's field-camera framing, logged (read-only)
        lss::player_two::location_rows::Install(*image);  // each camera's location rows from its own player
        lss::player_two::shoulder_scale::Install(*image);  // each half's shoulder offset scaled to its width
        lss::player_two::walk_limits::Install(*image);     // player 2's walk limits from player 2's own surroundings
        lss::player_two::handover_watch::Install(*image);  // the party order and the members' AI at hand-overs, logged
        lss::player_two::charapop_watch::Install(*image);  // the character detail near player 2, logged (read-only)
        lss::player_two::charapop_near::Install(*image);   // characters near player 2 keep the detail player 2's view gives them
        lss::player_two::charapop_centre::Install(*image); // the NPC population follows both players (Ctrl+F10 compares)
        lss::player_two::world_actor_reach::Install(*image); // world actors such as chests pop around player 2 too
        lss::player_two::ride_watch::Install(*image);      // chocobo and buggy rides, logged (read-only)
        lss::player_two::chocobo_ride::Install(*image);    // player 2 drives their own chocobo while the party rides
        lss::player_two::shared_ride::Install(*image);     // the buggy and the bronco: player 2 stands aside, every pad drives
        lss::player_two::wheelie_watch::Install(*image);   // the Wheelie system, logged (read-only)
        lss::player_two::wheelie_ride::Install(*image);    // player 2 boards a Wheelie of their own by walking into it
        lss::player_two::ride_probe::Install(*image);      // player 2's ride: sprint and homing, logged (read-only)
        lss::player_two::field_pickup::Install(*image);    // player 2's pickups: a second pass of the game's prompts
        lss::player_two::rumble_watch::Install(*image);    // logging only: each controller's rumble, pad by pad
        lss::player_two::pad_rumble::Install(*image);      // each player's rumble on their own pad
        lss::player_two::combat_rumble::Install(*image);   // player 2's combat rumble (hits, montage notifies) on player 2's pad
        lss::player_two::rumble_list_two::Install(*image);  // the traversal rumble list per player (zip line, chocobo, ropes)
        lss::player_two::audio_listener_two::Install(*image);  // player 2's sound: each SEAD sound from the nearer listener
        lss::player_two::chest_watch::Install(*image);     // chests, logged (read-only): presses, opens, grants
        lss::player_two::vending::Install(*image);         // player 2's vending machines: the shop's camera and pad
        lss::player_two::interact_watch::Install(*image);  // field interactions (pickups) per player, logged (read-only)
        lss::hud::InstallPlayerTwoPass(*image);      // player 2's HUD content, on (the hud-levers command switches it)
        lss::hud::InstallRootOverlayRender(*image);  // before the overlay renders: placement follow, trace
        lss::hud::prompt_rows::Install(*image);      // each player's field prompt rows in their own half
        lss::hud::field_markers::Install(*image);    // each player's point markers in their own half (after the opacity setter)
        lss::hud::action_icon_base::Install(*image); // the prompt rows keep their corner by their own base
        lss::hud::area_icons_two::Install(*image);   // player 2's own map icons and markers
        lss::hud::target_views::Install(*image);     // each half's cursor and enemy gauges follow its player's target

        lss::dev::Install(*image, *session_, layout_, singleView_, viewPointInstalled_);  // built with LSS_DEV_TOOLS only

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
            L"merges the pads");
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
        const bool left = session_->LeaveNow();
        FollowPlayerTwo();  // now, not at the next tick: the view module must not keep the old player 2
        // The viewport outlives the world: its split switch is cleared while player 1's controller
        // is still this world's.
        LogSingleViewChange(singleView_.Update(false, false));
        lss::views::SetSplitShown(false);
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
        lss::player_two::pad_rumble::BeforeTick();  // before the world tick: player 2's controller ordered after player 1's
        lss::player_two::combat_rumble::BeforeTick();  // outside every notify: their call sites; the status line
        lss::player_two::rumble_list_two::Tick();      // player 2's list made at join (dropped if player 2 went out another way)
        lss::player_two::audio_listener_two::Tick();  // the controller listener slot; the listeners and the summary (Debug)
        lss::player_two::chest_watch::Tick();          // an open followed from its press to its grant (logged)
        lss::player_two::vending::Tick();              // a press that opened no shop; player 2 leaving with their shop up
        if (!tickSeen_)
        {
            tickSeen_ = true;
            Log(L"engine tick callback is live");
        }
        switch (keys_.Poll())
        {
            case lss::players::PlayerIntent::Join: session_->Join(); break;
            case lss::players::PlayerIntent::Leave: session_->Leave(); break;  // logs its own outcome
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
        session_->SetSharedRide(shared);
        lss::player_two::pad_sticks::SetStoodDown(shared);
        lss::player_two::climb_context::Tick();  // this frame's input copies for the climbs on the anim worker (W5)
        session_->Tick();
        lss::dev::TickAfterSession();
        FollowPlayerTwo();
        lss::player_two::walk_limits::Tick();  // after the session's tick: a take's copy is overwritten this frame
        lss::player_two::handover_watch::Tick();

        // Player 1's single view for the game's whole-screen states (players/single_view.h), and while
        // player 2 waits for a member (only player 1 plays then; the user's decision, 2026-10-01): while
        // it holds, player 2 stays in but the split's presentation is off, as with one player.
        const bool playerTwoIn = session_->HasPlayerTwo();
        const bool waits       = PlayerTwoWaits();
        const bool asides      = session_->StandsAside() && singleView_.Mode() == lss::players::SingleViewMode::Auto;
        LogSingleViewChange(singleView_.Update(playerTwoIn, WholeScreenShown(playerTwoIn) || waits || asides));
        const bool splitShown = playerTwoIn && !singleView_.Single();
        lss::views::SetSplitShown(splitShown);
        lss::player_two::nav_strand::Tick(splitShown);  // a stranded player 2 is warped back to the party
        lss::player_two::camera_repair::Tick();
        lss::player_two::ride_framing_watch::Tick();
        lss::player_two::traversal_watch::Tick();
        lss::player_two::trigger_two::Tick();
        lss::player_two::traversal_camera::Tick();  // the traversal cameras' collision watch (Debug, on change)
        lss::player_two::near_fade::Tick();  // player 2's character's fade state (Debug, on change)
        lss::player_two::approach_two::Tick();  // the crossing walk-up calls, swapped once from here
        lss::player_two::move_type_two::Tick();  // player 2's deferred walk-cap clear, the layer-2 watch
        lss::player_two::vault_camera::Tick();  // player 2's vault cameras end on player 2's camera (lever L2)
        lss::player_two::chocobo_climb::Tick();  // player 2's climb camera's states and the summary (Debug)
        lss::player_two::camera_distance::TickFraming(playerTwoIn);  // per-layout distance steps (roadmap 2b)
        lss::player_two::shoulder_scale::Poll(splitShown);
        lss::player_two::framing_watch::Tick(splitShown);
        lss::player_two::charapop_watch::Tick(splitShown);
        lss::player_two::anim_two_views::Tick(splitShown);  // its frame count, before the world tick
        lss::player_two::charapop_near::Tick(splitShown, session_->PlaysMember());
        lss::player_two::charapop_centre::Tick(splitShown, session_->PlaysMember());
        lss::player_two::world_actor_reach::Tick(session_->PlaysMember());
        lss::player_two::ride_watch::Tick();
        lss::player_two::wheelie_watch::Tick();
        lss::player_two::ride_probe::Tick();
        lss::hud::navimap_watch::Tick(splitShown);
        lss::hud::minimap::Tick(splitShown);  // outside the overlay render, before it
        if (wholeScreen_)
        {
            lss::views::UpdateStreamViews(splitShown);  // needs the game-state check whole_screen verified
        }
        if (mainCameraShape_)
        {
            // Filled halves: each main camera's own 16:9 constraint, cleared once by the game's setter.
            lss::player_two::main_camera_shape::Update(lss::player_two::generation::Active() && splitShown &&
                                                       lss::views::FillsHalves());
        }
        LogSettingsChange(settings_, settings_.Update(playerTwoIn));
        // The timing method before the gap recorder goes off, and back after it returns.
        if (playerTwoIn)
        {
            LogSettingsChange(timingSettings_, timingSettings_.Update(true));
            LogSettingsChange(gapSettings_, gapSettings_.Update(true));
        }
        else
        {
            LogSettingsChange(gapSettings_, gapSettings_.Update(false));
            LogSettingsChange(timingSettings_, timingSettings_.Update(false));
        }
        if (playerTwoIn != submitHintsSkipped_)
        {
            submitHintsSkipped_ = playerTwoIn;
            lss::renderer::SetSubmitHintSkip(playerTwoIn);
        }
        LogLayoutChange(layout_.Update(playerTwoIn));
        UpdateHalfShape(splitShown);
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

    /// Keeps the views module's idea of the halves' shape current while player 2 is in: the
    /// viewport size (it changes with the window) and the layout (it changes by key or script).
    /// Read twice a second; until the first reading the split FOV leaves the stock rule in place.
    void UpdateHalfShape(bool splitShown) noexcept
    {
        if (!splitShown)
        {
            halfShapeTicks_ = 0;
            lss::views::SetHalfShape({});
            return;
        }
        if (halfShapeTicks_++ % kHalfShapeIntervalTicks != 0)
        {
            return;
        }
        const lss::platform::frame_watch::Job job(L"half shape read");
        auto*      playerOne  = static_cast<RC::Unreal::UObject*>(const_cast<void*>(session_->PlayerOne()));
        auto**     controller = lss::players::unreal::ControllerField(playerOne);
        const auto size   = controller != nullptr ? lss::players::unreal::ViewportSizeOf(*controller) : std::nullopt;
        const auto layout = lss::players::unreal::ReadTwoPlayerSplitscreenLayout();
        const auto half   = size && layout ? lss::views::HalfOf(size->width, size->height,
                                                                *layout == static_cast<std::uint8_t>(
                                                                               lss::players::SplitLayoutKind::SideBySide))
                                           : lss::views::HalfShape{};
        lss::views::SetHalfShape(half);
        if (half.aspect != loggedHalfAspect_)
        {
            loggedHalfAspect_ = half.aspect;
            if (half.aspect > 0.0f)
            {
                Log(L"split halves: viewport {}x{}, layout {}, half aspect {:.3f}, height {:.2f} of the screen",
                    size->width, size->height, *layout, half.aspect, half.heightRatio);
            }
            else
            {
                Log(L"split halves: the viewport size or layout cannot be read; the split FOV waits (stock rule meanwhile)");
            }
        }
    }

    void ToggleFill() noexcept
    {
        if (!viewPointInstalled_)
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

    static void LogLayoutChange(const lss::players::LayoutChange& change) noexcept
    {
        using Kind = lss::players::LayoutChange::Kind;
        const auto name = [](std::uint8_t value) {
            return value == static_cast<std::uint8_t>(lss::players::SplitLayoutKind::SideBySide) ? L"side by side"
                   : value == static_cast<std::uint8_t>(lss::players::SplitLayoutKind::TopBottom) ? L"top/bottom"
                                                                                                   : L"another value";
        };
        switch (change.kind)
        {
            case Kind::None: break;
            case Kind::Chosen: Log(L"split layout: {} from the next join", name(change.value)); break;
            case Kind::Unreadable:
                Log(L"split layout: TwoPlayerSplitscreenLayout cannot be read, so the switch did nothing");
                break;
            case Kind::Applied:
                Log(L"split layout: TwoPlayerSplitscreenLayout = {} ({})", change.value, name(change.value));
                break;
            case Kind::ApplyFailed:
                Log(L"split layout: could not set TwoPlayerSplitscreenLayout to {}; the game's layout stays", change.value);
                break;
            case Kind::Restored:
                Log(L"player 2 is out: TwoPlayerSplitscreenLayout restored to {} ({})", change.value, name(change.value));
                break;
            case Kind::LeftAlone:
                Log(L"player 2 is out: TwoPlayerSplitscreenLayout was changed to {} meanwhile; left as it is", change.value);
                break;
            case Kind::RestoreFailed:
                Log(L"player 2 is out: could not restore TwoPlayerSplitscreenLayout to {}", change.value);
                break;
        }
    }

    /// Keeps the view-point module's idea of the players current, and describes the local players
    /// when player 2 appears (a subclass with its own vtable would bypass the camera choice).
    void FollowPlayerTwo() noexcept
    {
        const void*         playerTwo = session_->PlayerTwo();
        const std::uint32_t joins     = session_->JoinCount();
        if (playerTwo == followedPlayerTwo_ && joins == followedJoins_)
        {
            return;
        }
        followedPlayerTwo_ = playerTwo;
        followedJoins_     = joins;
        // The FOV rule's offset, confirmed by reflection before the first split frame uses it.
        if (playerTwo != nullptr)
        {
            const auto axisOffset = lss::players::unreal::AspectRatioAxisConstraintOffset();
            lss::views::SetAxisConstraintOffset(axisOffset.value_or(0));
            if (!axisOffset)
            {
                Log(L"ULocalPlayer::AspectRatioAxisConstraint not found where the engine reads it; split views keep "
                    L"the game's FOV rule");
            }
        }
        // Through null first: a new player 2 at the old one's address must still start fresh
        // (on player 1's camera, first view point described).
        lss::views::SetPlayers(nullptr, nullptr);
        lss::views::SetPlayers(session_->PlayerOne(), playerTwo);
        if (playerTwo == nullptr || !viewPointInstalled_)
        {
            return;
        }
        auto* players = lss::players::unreal::LocalPlayers(lss::players::unreal::GameInstance());
        for (std::int32_t i = 0; players != nullptr && i < players->Num(); ++i)
        {
            auto* player = (*players)[i];
            Log(L"local player {}: class {}, view point {}", i + 1, player->GetClassPrivate()->GetName(),
                lss::views::UsesEngineViewPoint(player) ? L"through the engine's ULocalPlayer entry"
                                                         : L"through ANOTHER vtable (the camera choice cannot reach it)");
        }
    }

    void ToggleOwnCamera() noexcept
    {
        if (!viewPointInstalled_)
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
        return playerTwoIn && wholeScreen_ && singleView_.Mode() == lss::players::SingleViewMode::Auto &&
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
        using Kind = lss::players::SingleViewChange::Kind;
        switch (change.kind)
        {
            case Kind::None: break;
            case Kind::Single:
                Log(L"single view: player 1's view alone (SetForceDisableSplitscreen(true)); player 2 stays in");
                break;
            case Kind::AlreadySingle:
                Log(L"single view: the split was already force-disabled by someone else; used, and not cleared by the mod");
                break;
            case Kind::SingleFailed:
                Log(L"single view: the split switch could not be read or set; the split stays");
                break;
            case Kind::Split: Log(L"single view: over, the split is back (SetForceDisableSplitscreen(false))"); break;
            case Kind::LeftAlone:
                Log(L"single view: over; the split switch is not the mod's (or was cleared meanwhile), left as it is");
                break;
            case Kind::SplitFailed:
                Log(L"single view: the split switch could not be read or cleared; the screen stays single");
                break;
        }
    }

    static void LogSettingsChange(const lss::players::SplitScreenSettings& setting,
                                  const lss::players::SettingsChange& change) noexcept
    {
        using Kind = lss::players::SettingsChange::Kind;
        const wchar_t*     name  = setting.Name();
        const std::int32_t value = setting.Value();
        switch (change.kind)
        {
            case Kind::None: break;
            case Kind::Applied: Log(L"split screen: {} {} -> {} while player 2 is in", name, change.value, value); break;
            case Kind::AlreadySet: Log(L"split screen: {} is already {}; left as it is", name, value); break;
            case Kind::ApplyFailed:
                Log(L"split screen: could not set {} to {}; rendering stays correct but slower", name, value);
                break;
            case Kind::Restored: Log(L"player 2 is out: {} restored to {}", name, change.value); break;
            case Kind::LeftAlone:
                Log(L"player 2 is out: {} was changed to {} meanwhile; left as it is", name, change.value);
                break;
            case Kind::RestoreFailed: Log(L"player 2 is out: could not restore {} to {}", name, change.value); break;
        }
    }

    std::optional<lss::player_two::Session>   session_;
    lss::players::JoinKeys                    keys_;
    ULONGLONG                                 nextStatus_ = 0;
    lss::players::SplitScreenSettings         settings_{
        {&lss::players::unreal::ConsoleVariableInt, &lss::players::unreal::ExecuteConsoleCommand}};
    lss::players::SplitScreenSettings         timingSettings_{
        {&lss::players::unreal::ConsoleVariableInt, &lss::players::unreal::ExecuteConsoleCommand},
        lss::players::SplitScreenSettings::kTimingMethod, lss::players::SplitScreenSettings::kTimestampQueries};
    bool                                      submitHintsSkipped_ = false;  // the join/leave switch's last state
    lss::players::SplitScreenSettings         gapSettings_{
        {&lss::players::unreal::ConsoleVariableInt, &lss::players::unreal::ExecuteConsoleCommand},
        lss::players::SplitScreenSettings::kGapRecorder, lss::players::SplitScreenSettings::kGapRecorderOff};
    lss::players::SplitLayout                 layout_{{&lss::players::unreal::ReadTwoPlayerSplitscreenLayout,
                                                       &lss::players::unreal::WriteTwoPlayerSplitscreenLayout}};
    lss::players::SingleView                  singleView_{{&ReadSplitSwitch, &WriteSplitSwitch}};
    bool                                      playerTwoWaited_ = false;  // as last logged
    std::uint32_t                             halfShapeTicks_     = 0;
    float                                     loggedHalfAspect_   = -1.0f;
    bool                                      tickSeen_           = false;
    bool                                      rendererInstalled_  = false;
    bool                                      viewPointInstalled_ = false;
    bool                                      mainCameraShape_    = false;
    bool                                      wholeScreen_        = false;
    const void*                               followedPlayerTwo_  = nullptr;
    std::uint32_t                             followedJoins_      = 0;
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
