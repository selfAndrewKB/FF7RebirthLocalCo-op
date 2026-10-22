#include "modules.h"

#include "hud/action_icon_base.h"
#include "hud/area_icons_two.h"
#include "hud/field_markers.h"
#include "hud/layout_scale.h"
#include "hud/minimap.h"
#include "hud/minimap_once.h"
#include "hud/overlay_layout.h"
#include "hud/player_two_pass.h"
#include "hud/prompt_rows.h"
#include "hud/root_overlay_render.h"
#include "hud/search_pins.h"
#include "hud/second_image.h"
#include "hud/target_views.h"
#include "hud/user_widgets.h"
#include "hud/widget_opacity.h"
#include "player_two/anim_two_views.h"
#include "player_two/approach_two.h"
#include "player_two/attack_cameras.h"
#include "player_two/attack_framing.h"
#include "player_two/audio_listener_two.h"
#include "player_two/battle_camera.h"
#include "player_two/battle_warp_two.h"
#include "player_two/camera_distance.h"
#include "player_two/camera_effects_two.h"
#include "player_two/camera_override.h"
#include "player_two/camera_repair.h"
#include "player_two/charapop_centre.h"
#include "player_two/charapop_near.h"
#include "player_two/chocobo_climb.h"
#include "player_two/chocobo_ride.h"
#include "player_two/climb_context.h"
#include "player_two/combat_rumble.h"
#include "player_two/command_locks.h"
#include "player_two/command_menu_tick.h"
#include "player_two/command_menu_two.h"
#include "player_two/command_rules.h"
#include "player_two/equal_targeting.h"
#include "player_two/ffb_manager_two.h"
#include "player_two/field_pickup.h"
#include "player_two/leader_reads.h"
#include "player_two/location_rows.h"
#include "player_two/lock_group_coin.h"
#include "player_two/lock_on.h"
#include "player_two/main_camera_shape.h"
#include "player_two/move_type_two.h"
#include "player_two/nav_strand.h"
#include "player_two/near_fade.h"
#include "player_two/pad_rumble.h"
#include "player_two/party_switch.h"
#include "player_two/party_warp.h"
#include "player_two/player_hits.h"
#include "player_two/rumble_list_two.h"
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
#include "player_two/wheelie_ride.h"
#include "player_two/widget_owner.h"
#include "player_two/world_actor_reach.h"
#include "renderer/me_feedback_once.h"
#include "renderer/submit_hints.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"
#include "views/third_view.h"
#include "views/whole_screen.h"
#include "views/window_span.h"

#include <array>
#include <cstddef>
#include <type_traits>

namespace lss::modules {

namespace {

/// One row: the module's install, and its removal or none.
struct Module
{
    void (*install)(const platform::GameImage&) noexcept;
    void (*remove)() noexcept;  // nullptr: it stays installed (its code stays valid: the DLL is pinned)
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
    Module{Call<&views::InstallWholeScreen>, nullptr},
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
    // player 2's chocobo vaults without taking both ride framings
    Module{Call<&player_two::vault_camera::Install>, &player_two::vault_camera::Remove},
    // player 2's chocobo climbs walls, with its own climb camera
    Module{Call<&player_two::chocobo_climb::Install>, &player_two::chocobo_climb::Remove},

    // The HUD's layout in split views (hud/overlay_layout.h). Optional like the view point: without it the HUD keeps
    // the game's full-screen layout, cropped to each half; the layout scale (the contain rule) needs it.
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
    Module{Call<&player_two::field_pickup::Install>, nullptr},
    // each player's rumble on their own pad
    Module{Call<&player_two::pad_rumble::Install>, nullptr},
    // player 2's combat rumble (hits, montage notifies) on player 2's pad
    Module{Call<&player_two::combat_rumble::Install>, nullptr},
    // the traversal rumble list per player
    Module{Call<&player_two::rumble_list_two::Install>, &player_two::rumble_list_two::Remove},
    // player 2's own force-feedback manager
    Module{Call<&player_two::ffb_manager_two::Install>, &player_two::ffb_manager_two::Remove},
    // each SEAD sound from the nearer listener
    Module{Call<&player_two::audio_listener_two::Install>, &player_two::audio_listener_two::Remove},
    // player 2's vending machines: the shop's camera and pad
    Module{Call<&player_two::vending::Install>, nullptr},
    // player 2's shopkeepers: admit, press, the shop's AI event
    Module{Call<&player_two::shop_talk::Install>, nullptr},

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
    Module{Call<&player_two::attack_cameras::Install>, nullptr},
    // enemy specials: the target's half only; leader shots on the coin player
    Module{Call<&player_two::attack_framing::Install>, nullptr},
    // player 2's member's guard effect, shakes and chain pulls on player 2's camera
    Module{Call<&player_two::camera_effects_two::Install>, nullptr},
    // a synergy partner held by either human is handed to its AI and back
    Module{Call<&player_two::synergy_partner::Install>, &player_two::synergy_partner::Remove},

    // Player 2's HUD content, and each half's own prompts, markers, pins and targets.
    // player 2's HUD content, on (the hud-levers command switches it)
    Module{Call<&hud::InstallPlayerTwoPass>, &hud::RemovePlayerTwoPass},
    // before the overlay renders: placement follow, trace
    Module{Call<&hud::InstallRootOverlayRender>, &hud::RemoveRootOverlayRender},
    // the game's opacity setter, read by the rows, markers, pins, targets and the command menu's view
    Module{Call<&hud::widget_opacity::Install>, nullptr},
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
static_assert(Ordered<&player_two::climb_context::Install, &player_two::traversal_camera::Install>(),
              "climb_context before traversal_camera: the camera holders need its Ticks");
static_assert(Ordered<&views::InstallPlayerViewPoint, &views::InstallStreamViews>(),
              "the view point before the streaming views: they read whether it is installed");
static_assert(Ordered<&hud::InstallOverlayLayout, &hud::InstallLayoutScale>(),
              "the overlay layout before the layout scale: the contain rule needs the layout");
static_assert(Ordered<&hud::widget_opacity::Install, &hud::prompt_rows::Install>() &&
                  Ordered<&hud::widget_opacity::Install, &hud::field_markers::Install>() &&
                  Ordered<&hud::widget_opacity::Install, &hud::search_pins::Install>() &&
                  Ordered<&hud::widget_opacity::Install, &hud::target_views::Install>(),
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

}  // namespace lss::modules
