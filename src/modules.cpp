#include "modules.h"

#include "hud/battle/target_views.h"
#include "hud/field/action_icon_base.h"
#include "hud/field/field_markers.h"
#include "hud/field/prompt_rows.h"
#include "hud/field/search_pins.h"
#include "hud/layout/layout_scale.h"
#include "hud/layout/overlay_layout.h"
#include "hud/minimap/area_icons_two.h"
#include "hud/minimap/minimap.h"
#include "hud/minimap/minimap_once.h"
#include "hud/passes/player_two_pass.h"
#include "hud/passes/root_overlay_render.h"
#include "hud/passes/second_image.h"
#include "hud/user_widgets.h"
#include "player_two/battle/battle_camera.h"
#include "player_two/battle/battle_warp_two.h"
#include "player_two/battle/command_locks.h"
#include "player_two/battle/command_menu_tick.h"
#include "player_two/battle/command_menu_two.h"
#include "player_two/battle/command_rules.h"
#include "player_two/battle/equal_targeting.h"
#include "player_two/battle/leader_reads.h"
#include "player_two/battle/lock_group_coin.h"
#include "player_two/battle/lock_on.h"
#include "player_two/battle/player_hits.h"
#include "player_two/battle/synergy_partner.h"
#include "player_two/battle/target_coin.h"
#include "player_two/battle/widget_owner.h"
#include "player_two/camera/attack_cameras.h"
#include "player_two/camera/attack_framing.h"
#include "player_two/camera/camera_distance.h"
#include "player_two/camera/camera_effects_two.h"
#include "player_two/camera/camera_override.h"
#include "player_two/camera/camera_repair.h"
#include "player_two/camera/location_rows.h"
#include "player_two/camera/main_camera_shape.h"
#include "player_two/camera/near_fade.h"
#include "player_two/camera/shoulder_scale.h"
#include "player_two/camera/vault_camera.h"
#include "player_two/feedback/audio_listener_two.h"
#include "player_two/feedback/combat_rumble.h"
#include "player_two/feedback/ffb_manager_two.h"
#include "player_two/feedback/pad_rumble.h"
#include "player_two/feedback/rumble_list_two.h"
#include "player_two/generation.h"
#include "player_two/input/approach_two.h"
#include "player_two/input/climb_context.h"
#include "player_two/input/move_type_two.h"
#include "player_two/input/walk_limits.h"
#include "player_two/rides/chocobo_ride.h"
#include "player_two/rides/shared_ride.h"
#include "player_two/rides/wheelie_ride.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/session/leave_watch.h"
#include "player_two/session/party_switch.h"
#include "player_two/session/session.h"
#include "player_two/traversal/chocobo_climb.h"
#include "player_two/traversal/traversal_camera.h"
#include "player_two/traversal/traversal_watch.h"
#include "player_two/world/anim_two_views.h"
#include "player_two/world/charapop_centre.h"
#include "player_two/world/charapop_near.h"
#include "player_two/world/field_pickup.h"
#include "player_two/world/nav_strand.h"
#include "player_two/world/party_warp.h"
#include "player_two/world/shop_talk.h"
#include "player_two/world/switch_volume_two.h"
#include "player_two/world/trigger_two.h"
#include "player_two/world/vending.h"
#include "player_two/world/world_actor_reach.h"
#include "players/unreal_access.h"
#include "players/widget_opacity.h"
#include "renderer/family_boundary.h"
#include "renderer/me_feedback_once.h"
#include "renderer/submit_hints.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"
#include "views/third_view.h"
#include "views/whole_screen.h"
#include "views/window_span.h"

#if LSS_DEV
#include "control/mod_commands.h"
#include "hud/hud_trace.h"
#include "hud/position_probe.h"
#include "hud/widget_dump.h"
#include "platform/command_registry.h"
#include "platform/frame_watch.h"
#include "platform/log.h"
#include "player_two/session/field_exit.h"
#include "player_two/session/own_controller.h"
#include "player_two/session/party_member.h"
#include "players/console_access.h"
#include "players/single_view.h"
#include "players/split_layout.h"
#endif

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <type_traits>

namespace lss::modules {

namespace {

/// One row: the module's install, and its removal or none.
struct Module
{
    void (*install)(const platform::GameImage&) noexcept;
    void (*remove)() noexcept;  // nullptr: it patches nothing (its state stays; the DLL is pinned)
};

/// The row's install call: a module's Install returns bool or void (its outcome is its own log line) and takes
/// the game image or nothing.
template <auto Install>
void Call(const platform::GameImage& image) noexcept
{
    if constexpr (std::is_invocable_v<decltype(Install), const platform::GameImage&>)
    {
        (void)Install(image);
    }
    else
    {
        (void)Install();
    }
}

// The table. The order is the install order; a row's comment says what the module does for the players.
constexpr std::array kModules{
    // The party leader answering player 2's member for one of the game's calls: first, so the modules that check it
    // at their own install come after it; every other holder takes a scope at tick time (an unheld scope refuses).
    Module{Call<&player_two::leader_scope::Install>, nullptr},

    // Player 2's stages that are not the session's own, next to their siblings.
    // player 2's Battle operator follows player 1's
    Module{Call<&player_two::battle_camera::Install>, &player_two::battle_camera::Remove},
    // player 2's camera gets its own NotifyOverride module
    Module{Call<&player_two::camera_override::Install>, nullptr},
    // the Options camera distance reaches player 2's camera
    Module{Call<&player_two::camera_distance::Install>, &player_two::camera_distance::Remove},
    // player 2's targeting: the game's tick run again for player 2
    Module{Call<&player_two::lock_on::Install>, &player_two::lock_on::Remove},
    // player 1's switch skips player 2's member
    Module{Call<&player_two::party_switch::Install>, &player_two::party_switch::Remove},
    // player 1 cannot command player 2's member
    Module{Call<&player_two::command_rules::Install>, &player_two::command_rules::Remove},
    // battles leave player 2's member where it is
    Module{Call<&player_two::battle_warp_two::Install>, &player_two::battle_warp_two::Remove},
    // player 2's command controller: built, follows the game's
    Module{Call<&player_two::command_menu_two::Install>, &player_two::command_menu_two::Remove},
    // the battle's command locks reach it by the game's setters
    Module{Call<&player_two::command_locks::Install>, &player_two::command_locks::Remove},
    // ticked in battle; player 2's attack, guard and dodge
    Module{Call<&player_two::command_menu_tick::Install>, &player_two::command_menu_tick::Remove},
    // party-list cells and key guide tick in player 2's view
    Module{Call<&player_two::widget_owner::Install>, &player_two::widget_owner::Remove},
    // parallel passes without their own GPU submission (player 2 in)
    Module{Call<&renderer::InstallSubmitHintSkip>, &renderer::RemoveSubmitHintSkip},
    // one scenery-streaming hand-in per frame, player 1's family's
    Module{Call<&renderer::InstallMeFeedbackOnce>, &renderer::RemoveMeFeedbackOnce},

    // The view-point entry serves the filled halves and the choice of player 2's camera. The split screen runs
    // without it (halves kept 16:9 with bars, player 2 on its own camera); those keys then say they are unavailable.
    Module{Call<&views::InstallPlayerViewPoint>, &views::RemovePlayerViewPoint},
    // filled halves: each main camera's own 16:9 constraint
    Module{Call<&player_two::main_camera_shape::Install>, nullptr},
    // menus, the map, cutscenes: player 1's single view
    Module{Call<&views::InstallWholeScreen>, &views::RemoveWholeScreen},
    // each player's camera streams scenery detail (needs the view point)
    Module{Call<&views::InstallStreamViews>, nullptr},
    // a third full-size view for the multi-monitor measurement
    Module{Call<&views::third_view::Install>, nullptr},
    // the game window across its row of monitors (the engine's reshape)
    Module{Call<&views::window_span::Install>, nullptr},

    // Player 2 in the world: strands, cameras, traversals.
    // the game's follow warp, for a stranded player 2
    Module{Call<&player_two::party_warp::Install>, nullptr},
    // player 2's member stranded off the loaded navmesh
    Module{Call<&player_two::nav_strand::Install>, &player_two::nav_strand::Remove},
    // a non-finite pose on player 2's camera: the game's camera reset
    Module{Call<&player_two::camera_repair::Install>, nullptr},
    // ladders, climb spots, glide and jet for player 2
    Module{Call<&player_two::traversal_watch::Install>, &player_two::traversal_watch::Remove},
    // climbs on the anim worker read their own climber's input (W5); before traversal_camera, whose holders need
    // these Ticks
    Module{Call<&player_two::climb_context::Install>, &player_two::climb_context::Remove},
    // player 2's traversal gets the game's traversal cameras
    Module{Call<&player_two::traversal_camera::Install>, &player_two::traversal_camera::Remove},
    // player 1's camera never fades player 2's character out
    Module{Call<&player_two::near_fade::Install>, &player_two::near_fade::Remove},
    // walk-up traversals read player 2's stick in player 2's camera basis
    Module{Call<&player_two::approach_two::Install>, &player_two::approach_two::Remove},
    // player 2's walk-step cap ends on player 2's controller
    Module{Call<&player_two::move_type_two::Install>, &player_two::move_type_two::Remove},
    // characters near player 2 animate by the nearer camera
    Module{Call<&player_two::anim_two_views::Install>, &player_two::anim_two_views::Remove},
    // player 2's trigger read after player 1's; admitted actors' events
    Module{Call<&player_two::trigger_two::Install>, &player_two::trigger_two::Remove},
    // player 2 inside CharaPop switch volumes (the shopkeepers near player 2); its events come from trigger_two's
    // filter
    Module{Call<&player_two::switch_volume_two::Install>, nullptr},
    // player 2's chocobo vaults without taking both ride framings
    Module{Call<&player_two::vault_camera::Install>, &player_two::vault_camera::Remove},
    // player 2's chocobo climbs walls, with its own climb camera
    Module{Call<&player_two::chocobo_climb::Install>, &player_two::chocobo_climb::Remove},

    // The HUD's layout in split views (hud/layout/overlay_layout.h). Optional like the view point: without it the HUD
    // keeps the game's full-screen layout, cropped to each half; the layout scale (the contain rule) needs it.
    Module{Call<&hud::InstallOverlayLayout>, &hud::RemoveOverlayLayout},
    Module{Call<&hud::InstallLayoutScale>, &hud::RemoveLayoutScale},
    // HUD widgets as the engine constructs them
    Module{Call<&hud::user_widgets::Install>, &hud::user_widgets::Remove},
    // player 2's HUD image, on (the hud-image command switches it)
    Module{Call<&hud::InstallSecondImage>, &hud::RemoveSecondImage},
    // the minimap's map rendered by player 1's pass only
    Module{Call<&hud::InstallMinimapOnce>, &hud::RemoveMinimapOnce},
    // player 2's minimap in player 2's half
    Module{Call<&hud::minimap::Install>, &hud::minimap::Remove},
    // each camera's location rows from its own player
    Module{Call<&player_two::location_rows::Install>, &player_two::location_rows::Remove},
    // each half's shoulder offset scaled to its width
    Module{Call<&player_two::shoulder_scale::Install>, &player_two::shoulder_scale::Remove},
    // player 2's walk limits from player 2's own surroundings
    Module{Call<&player_two::walk_limits::Install>, nullptr},

    // The world around player 2 (these three give the raised characters back at unload, before the table's walk).
    // characters near player 2 keep the detail player 2's view gives them
    Module{Call<&player_two::charapop_near::Install>, nullptr},
    // the NPC population follows both players (Ctrl+F10 compares)
    Module{Call<&player_two::charapop_centre::Install>, nullptr},
    // world actors such as chests pop around player 2 too
    Module{Call<&player_two::world_actor_reach::Install>, nullptr},

    // Rides, pickups, rumble, sound, shops.
    // player 2 drives their own chocobo while the party rides
    Module{Call<&player_two::chocobo_ride::Install>, nullptr},
    // the buggy and the bronco: player 2 stands aside, every pad drives
    Module{Call<&player_two::shared_ride::Install>, nullptr},
    // player 2 boards a Wheelie of their own by walking into it
    Module{Call<&player_two::wheelie_ride::Install>, &player_two::wheelie_ride::Remove},
    // player 2's pickups: a second pass of the game's prompts
    Module{Call<&player_two::field_pickup::Install>, &player_two::field_pickup::Remove},
    // each player's rumble on their own pad
    Module{Call<&player_two::pad_rumble::Install>, &player_two::pad_rumble::Remove},
    // player 2's combat rumble (hits, montage notifies) on player 2's pad
    Module{Call<&player_two::combat_rumble::Install>, &player_two::combat_rumble::Remove},
    // the traversal rumble list per player
    Module{Call<&player_two::rumble_list_two::Install>, &player_two::rumble_list_two::Remove},
    // player 2's own force-feedback manager
    Module{Call<&player_two::ffb_manager_two::Install>, &player_two::ffb_manager_two::Remove},
    // each SEAD sound from the nearer listener
    Module{Call<&player_two::audio_listener_two::Install>, &player_two::audio_listener_two::Remove},
    // player 2's vending machines: the shop's camera and pad
    Module{Call<&player_two::vending::Install>, &player_two::vending::Remove},
    // player 2's shopkeepers: admit, press, the shop's AI event
    Module{Call<&player_two::shop_talk::Install>, &player_two::shop_talk::Remove},

    // The enemies' view of two players, after the session: its removal must come before the stages' (the Condition
    // selects stop taking the equal-targeting answer while member_movement's leader-test slot is still in place).
    // Condition enemies count either player as the UCPC
    Module{Call<&player_two::equal_targeting::Install>, &player_two::equal_targeting::Remove},
    // player 2's hits and the hits player 2 takes count as the player's
    Module{Call<&player_two::player_hits::Install>, &player_two::player_hits::Remove},
    // each enemy aims at player 1 or 2, a coin per action
    Module{Call<&player_two::target_coin::Install>, &player_two::target_coin::Remove},
    // lock groups (Stun Ray) prefer player 1 or 2 by each pick's coin
    Module{Call<&player_two::lock_group_coin::Install>, &player_two::lock_group_coin::Remove},
    // the game's own leader reads for an enemy follow its coin
    Module{Call<&player_two::leader_reads::Install>, &player_two::leader_reads::Remove},
    // battle camera sequences (grabs, specials) on each camera's own state
    Module{Call<&player_two::attack_cameras::Install>, &player_two::attack_cameras::Remove},
    // enemy specials: the target's half only; leader shots on the coin player
    Module{Call<&player_two::attack_framing::Install>, &player_two::attack_framing::Remove},
    // player 2's member's guard effect, shakes and chain pulls on player 2's camera
    Module{Call<&player_two::camera_effects_two::Install>, &player_two::camera_effects_two::Remove},
    // a synergy partner held by either human is handed to its AI and back
    Module{Call<&player_two::synergy_partner::Install>, &player_two::synergy_partner::Remove},

    // Player 2's HUD content, and each half's own prompts, markers, pins and targets.
    // player 2's HUD content, on (the hud-levers command switches it)
    Module{Call<&hud::InstallPlayerTwoPass>, &hud::RemovePlayerTwoPass},
    // before the overlay renders: placement follow, trace
    Module{Call<&hud::InstallRootOverlayRender>, &hud::RemoveRootOverlayRender},
    // the game's opacity setter, read by the rows, markers, pins, targets and the command menu's view
    Module{Call<&players::widget_opacity::Install>, nullptr},
    // each player's field prompt rows in their own half
    Module{Call<&hud::prompt_rows::Install>, nullptr},
    // each player's point markers in their own half
    Module{Call<&hud::field_markers::Install>, nullptr},
    // each player's search pins (ranged lock-on, hookshot) in their own half
    Module{Call<&hud::search_pins::Install>, nullptr},
    // the prompt rows keep their corner by their own base
    Module{Call<&hud::action_icon_base::Install>, nullptr},
    // player 2's own map icons and markers
    Module{Call<&hud::area_icons_two::Install>, &hud::area_icons_two::Remove},
    // each half's cursor and enemy gauges follow its player's target
    Module{Call<&hud::target_views::Install>, &hud::target_views::Remove},
};

/// The row of a module's Install, or the table's size if it has none.
template <auto Install>
constexpr std::size_t IndexOf() noexcept
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        if (kModules[i].install == &Call<Install>)
        {
            return i;
        }
    }
    return kModules.size();
}

/// Both modules are in the table, Before's row ahead of After's.
template <auto Before, auto After>
constexpr bool Ordered() noexcept
{
    return IndexOf<Before>() < IndexOf<After>() && IndexOf<After>() < kModules.size();
}

constexpr bool EachOnce() noexcept
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
    {
        for (std::size_t j = i + 1; j < kModules.size(); ++j)
        {
            if (kModules[i].install == kModules[j].install)
            {
                return false;
            }
        }
    }
    return true;
}

// The order constraints the modules carry. A row moved past its dependency fails the build here. The targeting
// group's place after the session, and the dev watches' (target_census reads equal_targeting) after the whole table,
// are the entry point's: it installs the session before the table and the dev tools after it.
static_assert(EachOnce(), "a module is in the table twice");
static_assert(IndexOf<&player_two::leader_scope::Install>() == 0,
              "the leader scope first: the modules that check it at their own install come after it");
static_assert(Ordered<&player_two::climb_context::Install, &player_two::traversal_camera::Install>(),
              "climb_context before traversal_camera: the camera holders need its Ticks");
static_assert(Ordered<&views::InstallPlayerViewPoint, &views::InstallStreamViews>(),
              "the view point before the streaming views: they read whether it is installed");
static_assert(Ordered<&hud::InstallOverlayLayout, &hud::InstallLayoutScale>(),
              "the overlay layout before the layout scale: the contain rule needs the layout");
static_assert(Ordered<&players::widget_opacity::Install, &hud::prompt_rows::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::field_markers::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::search_pins::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::target_views::Install>(),
              "the opacity setter before the HUD modules that read it");

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    for (const auto& module : kModules)
    {
        module.install(image);
    }
}

void Remove() noexcept
{
    for (auto row = kModules.rbegin(); row != kModules.rend(); ++row)
    {
        if (row->remove != nullptr)
        {
            row->remove();
        }
    }
}

void TickBeforeSession() noexcept
{
    // The tick's identities (the member's and player 1's pawn's weaks, the driven chocobo) before any module's.
    player_two::generation::BeginTick(player_two::chocobo_ride::DrivenChocobo());
    player_two::pad_rumble::BeforeTick();    // before the world tick: player 2's controller ordered after player 1's
    player_two::combat_rumble::BeforeTick();  // outside every notify: their call sites; the status line
    player_two::rumble_list_two::Tick();      // player 2's list made at join (dropped if player 2 left another way)
    player_two::equal_targeting::Tick();      // both players' types for the Condition selects (no leader scope held)
    player_two::target_coin::Tick();          // the blueprint natives' swaps once reflection is up; the summary
    player_two::ffb_manager_two::Tick();      // player 2's component on player 2's own manager, after its BeginPlay
    player_two::audio_listener_two::Tick();   // the controller listener slot; the listeners and the summary (Debug)
    player_two::leave_watch::Tick();          // the last leave's controller, followed until it is freed (logged)
    player_two::vending::Tick();              // a press that opened no shop; player 2 leaving with their shop up
    player_two::shop_talk::Tick();            // the shopkeeper verdicts queued by player 2's pass
    player_two::attack_cameras::Tick();       // the attack-camera status line (Debug)
    player_two::attack_framing::Tick();       // the attack-framing latches' clock and status line (Debug)
    player_two::camera_effects_two::Tick();   // the camera-effect status line (Debug)
    player_two::synergy_partner::Tick();      // humans' synergy partners: session, late give-back, status (Debug)
    // The code placed from the game thread outside the overlay render (platform/deferred_sites.h): its polls.
    renderer::PollFamilyBoundary();
    renderer::PollMeFeedbackOnce();
    hud::PollMinimapOnce();
    player_two::location_rows::Poll();
    player_two::location_rows::Tick();
}

void TickAfterSession(const Frame& frame) noexcept
{
    views::third_view::Tick(frame.playerTwoIn);   // leaves with player 2; follows player 1's view target
    views::window_span::Tick(frame.playerTwoIn);  // leaves with player 2; holds the window's rect
    player_two::walk_limits::Tick();  // after the session's tick: a take's copy is overwritten this frame
    player_two::nav_strand::Tick(frame.splitShown);  // a stranded player 2 is warped back to the party
    player_two::camera_repair::Tick();
    player_two::traversal_watch::Tick();
    player_two::trigger_two::Tick();
    player_two::switch_volume_two::Tick();  // player 2's volumes left once player 2 is gone; the summary
    player_two::traversal_camera::Tick();  // player 2's idle camera holders; the collision watch (dev)
    player_two::near_fade::Tick();         // player 2's character's fade state (Debug, on change)
    player_two::approach_two::Tick();      // the crossing walk-up calls, swapped once from here
    player_two::move_type_two::Tick();     // player 2's deferred walk-cap clear, the layer-2 watch
    player_two::vault_camera::Tick();      // player 2's vault cameras end on player 2's camera (lever L2)
    player_two::chocobo_climb::Tick();     // player 2's climb camera's states and the summary (Debug)
    player_two::camera_distance::TickFraming(frame.playerTwoIn);  // per-layout distance steps (roadmap 2b)
    player_two::shoulder_scale::Poll(frame.splitShown, views::SplitWidthRatio(false), views::SplitWidthRatio(true));
    player_two::anim_two_views::Tick(frame.splitShown);  // its frame count, before the world tick
    player_two::charapop_near::Tick(frame.splitShown, frame.playsMember);
    player_two::charapop_centre::Tick(frame.splitShown, frame.playsMember);
    player_two::world_actor_reach::Tick(frame.playsMember);
    hud::minimap::Tick(frame.splitShown);  // outside the overlay render, before it
    views::UpdateStreamViews(frame.splitShown);  // nothing until whole_screen's game-state check passed
    // Filled halves: each main camera's own 16:9 constraint, cleared once by the game's setter. FillsHalves is
    // false without the view-point entry (nothing shapes the halves then), so the cameras keep their constraint.
    player_two::main_camera_shape::Update(player_two::generation::Active() && frame.splitShown &&
                                          views::FillsHalves());
    renderer::FollowPlayerTwo(frame.playerTwoIn);  // the submit hints skipped while player 2 is in
}

void LeavePlayerTwo(player_two::Session& session, const wchar_t* why) noexcept
{
    views::third_view::Remove(why);
    session.Leave();
}

Result SetOwnCamera(const player_two::Session& session, bool on) noexcept
{
    if (!views::ViewPointInstalled())
    {
        return {false, L"the view-point entry is not installed"};
    }
    if (!session.HasPlayerTwo())
    {
        return {false, L"player 2 is not in (Ctrl+F7 adds one)"};
    }
    if (views::ShowsOwnCamera() != on)
    {
        views::ToggleOwnCamera();
    }
    return {true, views::ShowsOwnCamera() ? L"player 2's own camera" : L"player 1's camera"};
}

Result SetFill(bool on) noexcept
{
    if (!views::ViewPointInstalled())
    {
        return {false, L"the view-point entry is not installed"};
    }
    if (views::FillsHalves() != on)
    {
        views::ToggleFill();
    }
    return {true, views::FillsHalves() ? L"halves filled" : L"16:9 halves with bars"};
}

Result SetThirdView(bool on, const wchar_t* why) noexcept
{
    if (on == views::third_view::In())
    {
        return {true, on ? L"the third view is already in" : L"the third view is not in"};
    }
    if (on)
    {
        return views::third_view::Add() ? Result{true, L"the third view is in"}
                                        : Result{false, L"the third view could not be added (the log says why)"};
    }
    views::third_view::Remove(why);
    return views::third_view::In() ? Result{false, L"the third local player could not be removed"}
                                   : Result{true, L"the third view is out"};
}

Result SetWindowSpan(const player_two::Session& session, bool on, const wchar_t* why) noexcept
{
    if (!views::window_span::Installed())
    {
        return {false, L"the window span is not installed (see the startup log)"};
    }
    if (on == views::window_span::In())
    {
        return {true, on ? L"the window is already spanned" : L"the window is not spanned"};
    }
    if (!on)
    {
        views::window_span::Leave(why);
        return views::window_span::In() ? Result{false, L"the window could not be put back"}
                                        : Result{true, L"the window put back"};
    }
    if (!session.HasPlayerTwo())
    {
        return {false, L"player 2 is not in (Ctrl+F7 first)"};
    }
    auto*      players = players::unreal::LocalPlayers(players::unreal::GameInstance());
    const auto columns = players == nullptr ? 0 : players->Num();
    if (!views::window_span::Enter(columns))
    {
        return {false, L"the window could not be spanned (the log says why)"};
    }
    return {true, std::format(L"{} columns", columns)};
}

#if LSS_DEV

namespace {

control::Outcome From(Result done)
{
    return {done.ok, std::move(done.text)};
}

/// The same readiness the session's rejoin waits for: the field running (the entry steps done), the game's battle
/// system and option data built, player 1's controller and camera manager on the End classes; and nothing over the
/// field (a menu, a scene, the title: its scene is a field with a one-member party, which the first three tests
/// accept; a run of 2026-10-21 ran every script at the title that way). The engine refuses a join from the game's
/// Main Menu (CreatePlayer returns without a player; seen 2026-10-22, the menu up after the load). Here because it
/// reads player 2's modules and the views both.
bool FieldReady() noexcept
{
    return player_two::field_exit::FieldReady() && player_two::party_member::GameReady() &&
           player_two::own_controller::Ready() &&
           views::ReasonOf(views::ReadScreenSignals()) == views::WholeScreenReason::None;
}

/// The commands of the shared actions above, and the field's readiness.
void RegisterOwnCommands(player_two::Session& session)
{
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name = L"leave", .help = L"remove player 2 (as Ctrl+F8); `wait-until p2.in == off` confirms it",
              .set  = [&session](const Value&) {
                  if (!session.HasPlayerTwo() && !session.AwaitingMember())
                  {
                      return control::Outcome{false, L"player 2 is not in"};
                  }
                  LeavePlayerTwo(session, L"player 2 leaves (the leave command)");  // over the next ticks
                  return control::Outcome{};
              }});
    Register({.name = L"own-camera", .shape = Shape::OnOff,
              .help = L"player 2's half shows their own camera (on), or player 1's",
              .set  = [&session](const Value& v) { return From(SetOwnCamera(session, std::get<bool>(v))); },
              .get  = [] { return Value{views::ShowsOwnCamera()}; }});
    Register({.name = L"bars", .shape = Shape::OnOff, .help = L"split views keep 16:9 with bars, or fill their halves",
              .set  = [](const Value& v) { return From(SetFill(!std::get<bool>(v))); },  // bars on: no fill
              .get  = [] { return Value{!views::FillsHalves()}; }});
    Register({.name = L"third-view", .shape = Shape::OnOff,
              .help = L"a third full-size view (a third local player on player 1's camera, three columns) for the "
                      L"multi-monitor measurement; needs player 2 in",
              .set  = [](const Value& v) { return From(SetThirdView(std::get<bool>(v), L"the third-view command")); },
              .get  = [] { return Value{views::third_view::In()}; }});
    Register({.name = L"window-span", .shape = Shape::OnOff,
              .help = L"the game window across its row of equal monitors, one column per local player, through the "
                      L"engine's own window reshape; needs player 2 in",
              .set  = [&session](const Value& v) {
                  return From(SetWindowSpan(session, std::get<bool>(v), L"the window-span command"));
              },
              .get = [] { return Value{views::window_span::In()}; }});
    Register({.name = L"field.ready", .shape = Shape::OnOff,
              .help = L"a save's field is loaded and playable, nothing over it (no menu, scene or title)",
              .get  = [] { return Value{FieldReady()}; }});
}

}  // namespace

void RegisterCommands(player_two::Session& session, players::SplitLayout& layout,
                      players::SingleView& singleView) noexcept
{
    try
    {
        // Bottom layer first, so `help` lists the commands from the log level up to the game's modules.
        platform::RegisterLogCommands();
        platform::frame_watch::RegisterCommands();
        players::console::RegisterCommands([&session] { return session.HasPlayerTwo(); });
        players::RegisterCommands(layout);
        players::RegisterCommands(singleView);
        player_two::RegisterCommands(session);
        views::RegisterViewPointCommands();
        views::RegisterStreamCommands();
        hud::RegisterOverlayLayoutCommands();
        hud::RegisterSecondImageCommands();
        hud::RegisterPlayerTwoPassCommands();
        hud::area_icons_two::RegisterCommands();
        hud::RegisterMinimapOnceCommands();
        hud::RegisterHudTraceCommands();
        hud::RegisterPositionProbeCommands();
        hud::RegisterDumpCommands();
        RegisterOwnCommands(session);
        control::RegisterModCommands();  // the groups not moved yet (phase 8, rows 6 and 7)
    }
    catch (...)
    {
        platform::Log(L"control: registering the commands failed (out of memory?); the table is incomplete");
    }
}

#endif

}  // namespace lss::modules
