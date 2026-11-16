#include "modules.h"

#include "battle/battle_camera.h"
#include "battle/battle_rates.h"
#include "battle/battle_warps.h"
#include "battle/command_locks.h"
#include "battle/command_menu.h"
#include "battle/command_menu_tick.h"
#include "battle/command_rules.h"
#include "battle/enemy_leader.h"
#include "battle/equal_targeting.h"
#include "battle/leader_reads.h"
#include "battle/lock_group_coin.h"
#include "battle/lock_on.h"
#include "battle/menu_tint.h"
#include "battle/player_hits.h"
#include "battle/synergy_partner.h"
#include "battle/target_coin.h"
#include "battle/widget_owner.h"
#include "camera/attack_cameras.h"
#include "camera/attack_framing.h"
#include "camera/camera_distance.h"
#include "camera/camera_override.h"
#include "camera/camera_repair.h"
#include "camera/location_rows.h"
#include "camera/main_camera_shape.h"
#include "camera/near_fade.h"
#include "camera/seat_camera_effects.h"
#include "camera/shoulder_scale.h"
#include "camera/vault_camera.h"
#include "feedback/combat_rumble.h"
#include "feedback/pad_rumble.h"
#include "feedback/seat_ffb_managers.h"
#include "feedback/sound_listeners.h"
#include "feedback/traversal_rumble.h"
#include "hud/battle/target_views.h"
#include "hud/field/action_icon_base.h"
#include "hud/field/field_markers.h"
#include "hud/field/jet_gauges.h"
#include "hud/field/prompt_rows.h"
#include "hud/field/search_pins.h"
#include "hud/field/wrangle_targets.h"
#include "hud/layout/hud_placement.h"
#include "hud/layout/layout_scale.h"
#include "hud/layout/overlay_layout.h"
#include "hud/minimap/minimap.h"
#include "hud/minimap/minimap_once.h"
#include "hud/minimap/column_area_icons.h"
#include "hud/passes/column_hud_images.h"
#include "hud/passes/overlay_pass.h"
#include "hud/passes/root_overlay_render.h"
#include "hud/passes/seat_hud_content.h"
#include "hud/passes/ui_effect_pass.h"
#include "hud/user_widgets.h"
#include "input/approach_own_camera.h"
#include "input/climb_context.h"
#include "input/pad_presence.h"
#include "input/pad_table.h"
#include "hud/save_notice.h"
#include "hud/view_prompt.h"
#include "hud/view_window.h"
#include "session/player_window.h"
#include "input/seat_walk_cap.h"
#include "input/walk_limits.h"
#include "platform/command_registry.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/generation.h"
#include "players/local_players.h"
#include "players/split_layout.h"
#include "players/unreal_access.h"
#include "players/widget_opacity.h"
#include "renderer/family_boundary.h"
#include "renderer/me_feedback_once.h"
#include "renderer/submit_hints.h"
#include "rides/chocobo_exits.h"
#include "rides/chocobo_ride.h"
#include "rides/shared_ride.h"
#include "rides/wheelie_ride.h"
#include "scopes/leader_scope.h"
#include "session/field_exit.h"
#include "session/leave_watch.h"
#include "session/own_controller.h"
#include "session/party_member.h"
#include "session/party_switch.h"
#include "session/session.h"
#include "traversal/chocobo_climb.h"
#include "traversal/chocobo_spots.h"
#include "traversal/cart_climb.h"
#include "traversal/hooks.h"
#include "traversal/traversal_camera.h"
#include "traversal/traversal_watch.h"
#include "views/column_layout.h"
#include "views/columns.h"
#include "views/mirror_player.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"
#include "views/whole_screen.h"
#include "views/window_span.h"
#include "world/anim_views.h"
#include "world/charapop_centre.h"
#include "world/charapop_near.h"
#include "world/field_pickup.h"
#include "world/levers.h"
#include "world/nav_strand.h"
#include "menu/coop_menu.h"
#include "world/party_warp.h"
#include "world/seat_switch_volumes.h"
#include "world/seat_triggers.h"
#include "world/shooting_targets.h"
#include "world/story_boxes.h"
#include "world/catch_up_move.h"
#include "world/run_catch_up.h"
#include "battle/arena_warp.h"
#include "hud/lead_mark.h"
#include "hud/low_health.h"
#include "world/shop_talk.h"
#include "world/vending.h"
#include "world/world_actor_reach.h"

#if LSS_DEV
#include "hud/hud_trace.h"
#include "hud/position_probe.h"
#include "hud/widget_dump.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/frame_watch.h"
#include "players/console_access.h"
#include "renderer/submission_census.h"
#include "renderer/view_family_submission.h"
#include "scopes/scope_leaks.h"
#endif

#include <algorithm>
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
    // The party leader answering a seat's member for one of the game's calls: first, so the modules that check it
    // at their own install come after it; every other holder takes a scope at tick time (an unheld scope refuses).
    Module{Call<&scopes::leader_scope::Install>, nullptr},
    // which physical pad answers each player's slot (identity until changed); a pad no player holds never pauses
    Module{Call<&input::pad_table::Install>, &input::pad_table::Remove},

    // The seats' stages that are not the session's own, next to their siblings.
    // each seat's Battle operator follows player 1's
    Module{Call<&battle::battle_camera::Install>, &battle::battle_camera::Remove},
    // each seat's camera gets its own NotifyOverride module
    Module{Call<&camera::camera_override::Install>, nullptr},
    // the Options camera distance reaches every seat's camera
    Module{Call<&camera::camera_distance::Install>, &camera::camera_distance::Remove},
    // each seat's targeting: the game's tick run again for the seat
    Module{Call<&battle::lock_on::Install>, &battle::lock_on::Remove},
    // every player's switch takes only a free AI member; a downed player's own switch runs
    Module{Call<&session::party_switch::Install>, &session::party_switch::Remove},
    // player 1 cannot command a seat's member
    Module{Call<&battle::command_rules::Install>, &battle::command_rules::Remove},
    // who a battle warps at its start and end, by the setting
    Module{Call<&battle::battle_warps::Install>, &battle::battle_warps::Remove},
    Module{Call<&battle::battle_rates::Install>, &battle::battle_rates::Remove},
    // each seat's command controller: built, follows the game's
    Module{Call<&battle::command_menu::Install>, &battle::command_menu::Remove},
    // the battle's command locks reach it by the game's setters
    Module{Call<&battle::command_locks::Install>, &battle::command_locks::Remove},
    // ticked in battle; each seat's attack, guard and dodge
    Module{Call<&battle::command_menu_tick::Install>, &battle::command_menu_tick::Remove},
    // the low-health red edge in each player's own view, on that player's own danger (after the controllers' tick)
    Module{Call<&hud::low_health::Install>, &hud::low_health::Remove},
    // party-list cells and key guide tick in their owner's view
    Module{Call<&battle::widget_owner::Install>, &battle::widget_owner::Remove},
    // parallel passes without their own GPU submission (several views)
    Module{Call<&renderer::InstallSubmitHintSkip>, &renderer::RemoveSubmitHintSkip},
    // one scenery-streaming hand-in per frame, player 1's family's
    Module{Call<&renderer::InstallMeFeedbackOnce>, &renderer::RemoveMeFeedbackOnce},

    // The view-point entry serves the filled halves and the choice of the seats' cameras. The split screen runs
    // without it (halves kept 16:9 with bars, each seat on its own camera); those keys then say they are unavailable.
    Module{Call<&views::InstallPlayerViewPoint>, &views::RemovePlayerViewPoint},
    // filled halves: each main camera's own 16:9 constraint
    Module{Call<&camera::main_camera_shape::Install>, nullptr},
    // menus, the map, cutscenes: player 1's single view
    Module{Call<&views::InstallWholeScreen>, &views::RemoveWholeScreen},
    // each player's camera streams scenery detail (needs the view point)
    Module{Call<&views::InstallStreamViews>, nullptr},
    // player 1's picture in every column no player has (the mirror views)
    Module{Call<&views::mirror_player::Install>, nullptr},
    // the game window across its row of monitors (the engine's reshape)
    Module{Call<&views::window_span::Install>, nullptr},
    // each column its monitor's width on a span of unequal monitors (the engine's layout table)
    Module{Call<&views::column_layout::Install>, &views::column_layout::Remove},
    // a Co-op row in the pause menu opening the game's Options screen with the settings table's options
    Module{Call<&menu::coop_menu::Install>, &menu::coop_menu::Remove},

    // The seats' players in the world: strands, cameras, traversals.
    // the game's follow warp, for a seat's stranded player
    Module{Call<&world::party_warp::Install>, nullptr},
    // a seat's member stranded off the loaded navmesh
    Module{Call<&world::nav_strand::Install>, &world::nav_strand::Remove},
    // a non-finite pose on a seat's camera: the game's camera reset
    Module{Call<&camera::camera_repair::Install>, nullptr},
    // ladders and climb spots for players 2 and 3 (logging)
    Module{Call<&traversal::traversal_watch::Install>, &traversal::traversal_watch::Remove},
    // climbs on the anim worker read their own climber's input (W5); before traversal_camera, whose holders need
    // these Ticks
    Module{Call<&input::climb_context::Install>, &input::climb_context::Remove},
    // a seat's traversal gets the game's traversal cameras
    Module{Call<&traversal::traversal_camera::Install>, &traversal::traversal_camera::Remove},
    // player 1's camera never fades a seat's character out
    Module{Call<&camera::near_fade::Install>, &camera::near_fade::Remove},
    // walk-up traversals read a seat's stick in the seat's camera basis
    Module{Call<&input::approach_own_camera::Install>, &input::approach_own_camera::Remove},
    // a seat's walk-step cap ends on the seat's controller
    Module{Call<&input::seat_walk_cap::Install>, &input::seat_walk_cap::Remove},
    // characters near a seat's player animate by the nearer camera
    Module{Call<&world::anim_views::Install>, &world::anim_views::Remove},
    // each seat's trigger read after player 1's; admitted actors' events
    Module{Call<&world::seat_triggers::Install>, &world::seat_triggers::Remove},
    // a seat's player inside CharaPop switch volumes (the shopkeepers near them); its events come from seat_triggers'
    // filter
    Module{Call<&world::seat_switch_volumes::Install>, nullptr},
    // a seat's member walking into its own story box ahead of the story waits for the story's moment
    Module{Call<&world::story_boxes::Install>, &world::story_boxes::Remove},
    // the game's own point query and warp, for a character brought beside player 1's
    Module{Call<&world::catch_up_move::Install>, nullptr},
    // a character taken past its own story box brought back for the level's walk through it
    Module{Call<&world::run_catch_up::Install>, nullptr},
    // L3 twice in a fight brings a character shut outside the fight's area in (the game's gather, one member)
    Module{Call<&battle::arena_warp::Install>, nullptr},
    // the stop of a walk player 2 or 3 keeps (lead-holder keeps), marked in that player's view only
    Module{Call<&hud::lead_mark::Install>, &hud::lead_mark::Remove},
    // a seat's chocobo vaults without taking both ride framings
    Module{Call<&camera::vault_camera::Install>, &camera::vault_camera::Remove},
    // a seat's chocobo climbs walls, with its own climb camera
    Module{Call<&traversal::chocobo_climb::Install>, &traversal::chocobo_climb::Remove},
    // a seat's chocobo takes the launch pads, jump boxes and sliders in the seat's scope
    Module{Call<&traversal::chocobo_spots::Install>, &traversal::chocobo_spots::Remove},
    // a seat's chocobo leaves its glide and water jet in the seat's scope, whoever ends them
    Module{Call<&rides::chocobo_exits::Install>, &rides::chocobo_exits::Remove},
    // the grappling gun's points and the hook ropes for the seats (their moves' ticks are the climb context's)
    Module{Call<&traversal::hooks::Install>, &traversal::hooks::Remove},
    // the listed switches for the seats: the point, the press latch and the in-range byte every player's, the hold
    // their own
    Module{Call<&world::levers::Install>, &world::levers::Remove},
    // the mine-cart climbs for the seats: the shared climb list keeps a cart while any player wishes it on
    Module{Call<&traversal::cart_climb::Install>, &traversal::cart_climb::Remove},

    // The HUD's layout in split views (hud/layout/overlay_layout.h). Optional like the view point: without it the HUD
    // keeps the game's full-screen layout, cropped to each half; the layout scale (the contain rule) needs it.
    Module{Call<&hud::InstallOverlayLayout>, &hud::RemoveOverlayLayout},
    Module{Call<&hud::InstallLayoutScale>, &hud::RemoveLayoutScale},
    // HUD widgets as the engine constructs them
    Module{Call<&hud::user_widgets::Install>, &hud::user_widgets::Remove},
    // each seat's HUD image, on (the hud-image command switches it)
    Module{Call<&hud::InstallColumnHudImages>, &hud::RemoveColumnHudImages},
    // The UI particles (the summon gauge's glow) drawn through each column's band map (hud/passes/ui_effect_pass.h):
    // the maps come from the layout above, the column from the HUD images' composite redirect, which must still run
    // while this one is taken down (removal runs in reverse: this row goes first).
    Module{Call<&hud::ui_effect_pass::Install>, &hud::ui_effect_pass::Remove},
    // the minimap's map rendered by player 1's pass only
    Module{Call<&hud::InstallMinimapOnce>, &hud::RemoveMinimapOnce},
    // each seat's minimap in the seat's view
    Module{Call<&hud::minimap::Install>, &hud::minimap::Remove},
    // each camera's location rows from its own player
    Module{Call<&camera::location_rows::Install>, &camera::location_rows::Remove},
    // each half's shoulder offset scaled to its width
    Module{Call<&camera::shoulder_scale::Install>, &camera::shoulder_scale::Remove},
    // each seat's walk limits from its own surroundings
    Module{Call<&input::walk_limits::Install>, nullptr},

    // The world around the seats' players (these three give the raised characters back at unload, before the
    // table's walk).
    // characters near a seat's player keep the detail that player's view gives them
    Module{Call<&world::charapop_near::Install>, nullptr},
    // the NPC population follows every player (Ctrl+F10 compares)
    Module{Call<&world::charapop_centre::Install>, nullptr},
    // world actors such as chests pop around a seat's player too
    Module{Call<&world::world_actor_reach::Install>, nullptr},

    // Rides, pickups, rumble, sound, shops.
    // each seat's player drives their own chocobo while the party rides
    Module{Call<&rides::chocobo_ride::Install>, nullptr},
    // the buggy and the bronco: the seats' players stand aside, every pad drives
    Module{Call<&rides::shared_ride::Install>, nullptr},
    // a seat's player boards a Wheelie of their own by walking into it
    Module{Call<&rides::wheelie_ride::Install>, &rides::wheelie_ride::Remove},
    // each seat's pickups: one more pass of the game's prompts
    Module{Call<&world::field_pickup::Install>, &world::field_pickup::Remove},
    // a breakable box counts as near for a seat's throw by that player's own distance
    Module{Call<&world::shooting_targets::Install>, &world::shooting_targets::Remove},
    // each player's rumble on their own pad
    Module{Call<&feedback::pad_rumble::Install>, &feedback::pad_rumble::Remove},
    // a seat's combat rumble (hits, montage notifies) on the seat's pad
    Module{Call<&feedback::combat_rumble::Install>, &feedback::combat_rumble::Remove},
    // the traversal rumble list per player
    Module{Call<&feedback::traversal_rumble::Install>, &feedback::traversal_rumble::Remove},
    // each seat's own force-feedback manager
    Module{Call<&feedback::seat_ffb_managers::Install>, &feedback::seat_ffb_managers::Remove},
    // each SEAD sound from the nearest player's listener
    Module{Call<&feedback::sound_listeners::Install>, &feedback::sound_listeners::Remove},
    // a seat's vending machines: the shop's camera and pad
    Module{Call<&world::vending::Install>, &world::vending::Remove},
    // a seat's shopkeepers: admit, press, the shop's AI event
    Module{Call<&world::shop_talk::Install>, &world::shop_talk::Remove},

    // The enemies' view of several players, after the session: its removal must come before the stages' (the Condition
    // selects stop taking the equal-targeting answer while member_movement's leader-test slot is still in place).
    // Condition enemies count any player as the UCPC
    Module{Call<&battle::equal_targeting::Install>, &battle::equal_targeting::Remove},
    // the seats' players' hits and the hits they take count as the player's
    Module{Call<&battle::player_hits::Install>, &battle::player_hits::Remove},
    // each enemy aims at one of the players, a coin per action
    Module{Call<&battle::target_coin::Install>, &battle::target_coin::Remove},
    // lock groups (Stun Ray) prefer one of the players by each pick's coin
    Module{Call<&battle::lock_group_coin::Install>, &battle::lock_group_coin::Remove},
    // the game's own leader reads for an enemy follow its coin
    Module{Call<&battle::leader_reads::Install>, &battle::leader_reads::Remove},
    // enemies turn on a standing member while player 1's character is down (the game's own leader-change event)
    Module{Call<&battle::enemy_leader::Install>, nullptr},
    // battle camera sequences (grabs, specials) on each camera's own state
    Module{Call<&camera::attack_cameras::Install>, &camera::attack_cameras::Remove},
    // enemy specials: the target's half only; leader shots on the coin player
    Module{Call<&camera::attack_framing::Install>, &camera::attack_framing::Remove},
    // a seat's member's guard effect, shakes and chain pulls on the seat's camera
    Module{Call<&camera::seat_camera_effects::Install>, &camera::seat_camera_effects::Remove},
    // a synergy partner held by either human is handed to its AI and back
    Module{Call<&battle::synergy_partner::Install>, &battle::synergy_partner::Remove},

    // The seats' HUD content, and each half's own prompts, markers, pins and targets.
    // each seat's HUD content, on (the hud-levers command switches it)
    Module{Call<&hud::InstallSeatHudContent>, &hud::RemoveSeatHudContent},
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
    // the chocobo wrangle's bubbles and reticle placed from each view's camera
    Module{Call<&hud::wrangle_targets::Install>, nullptr},
    // each player's chocobo's jet gauge placed from their own view, shown there only
    Module{Call<&hud::jet_gauges::Install>, &hud::jet_gauges::Remove},
    // the prompt rows keep their corner by their own base
    Module{Call<&hud::action_icon_base::Install>, nullptr},
    // each further column's own map icons and markers
    Module{Call<&hud::column_area_icons::Install>, &hud::column_area_icons::Remove},
    // a line of the mod's on the game's save-data notice (the title screen's one notice: which controller plays player 1)
    Module{Call<&hud::save_notice::Install>, &hud::save_notice::Remove},
    // a few lines in one player's own view, in the engine's font (the player windows)
    Module{Call<&hud::view_prompt::Install>, &hud::view_prompt::Remove},
    // each player's own window by pad: Start on a spare pad joins, View held opens it, B held leaves
    Module{Call<&session::player_window::Install>, &session::player_window::Remove},
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
static_assert(IndexOf<&scopes::leader_scope::Install>() == 0,
              "the leader scope first: the modules that check it at their own install come after it");
static_assert(Ordered<&input::climb_context::Install, &traversal::traversal_camera::Install>(),
              "climb_context before traversal_camera: the camera holders need its Ticks");
static_assert(Ordered<&input::climb_context::Install, &traversal::hooks::Install>(),
              "climb_context before hooks: the gun and the ropes need their moves' Ticks placed");
static_assert(Ordered<&views::InstallPlayerViewPoint, &views::InstallStreamViews>(),
              "the view point before the streaming views: they read whether it is installed");
static_assert(Ordered<&hud::InstallOverlayLayout, &hud::InstallLayoutScale>(),
              "the overlay layout before the layout scale: the contain rule needs the layout");
static_assert(Ordered<&input::pad_table::Install, &session::player_window::Install>() &&
                  Ordered<&hud::view_prompt::Install, &session::player_window::Install>(),
              "the pad table and the prompt before the player windows: the windows listen to the one, draw through the other");
static_assert(Ordered<&players::widget_opacity::Install, &hud::prompt_rows::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::field_markers::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::search_pins::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::jet_gauges::Install>() &&
                  Ordered<&players::widget_opacity::Install, &hud::target_views::Install>(),
              "the opacity setter before the HUD modules that read it");

// Game thread: the columns as the last columns' tick read them (TickColumns), and by seat the column showing that
// player's own view (TickAfterSession: views/column_source.h OwnColumn, -1 for none), for the calls before them: each
// seat's sound listener follows it.
views::columns::Map                   g_lastColumns;
std::array<int, players::kMaxPlayers> g_ownColumns{-1, -1, -1};

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
    // The driven chocobo by seat, each built seat's (index 0, player 1's, unused).
    const void* chocobos[players::kMaxPlayers] = {};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        chocobos[seat] = rides::chocobo_ride::DrivenChocobo(seat);
    }
    players::generation::BeginTick(chocobos);
    input::pad_presence::Tick();  // which XInput users answer, once a second; the pad table's re-check from it
    menu::coop_menu::Tick();      // the Co-op row placed on the first tick; the page's changes; its close watched
    feedback::pad_rumble::BeforeTick();   // before the world tick: each seat's controller ordered after player 1's
    feedback::combat_rumble::BeforeTick();  // outside every notify: their call sites; the status line
    feedback::traversal_rumble::Tick();      // each seat's list made at join (dropped if it left another way)
    battle::equal_targeting::Tick();      // every player's types for the Condition selects (no leader scope held)
    battle::target_coin::Tick();          // the blueprint natives' swaps once reflection is up; the summary
    battle::enemy_leader::Tick();         // whom enemies follow while player 1's character is down (after the types)
    feedback::seat_ffb_managers::Tick();      // each seat's component on its own manager, after its BeginPlay
    // the controller listener slot; each seat's listener on the column showing its own view (the last
    // tick's sources, Shown::OwnColumn), off while it has none; the summary (Debug)
    feedback::sound_listeners::Tick(g_ownColumns);
    session::leave_watch::Tick();          // the last leave's controller, followed until it is freed (logged)
    world::vending::Tick();              // a press that opened no shop; a seat's player leaving with their shop up
    world::shop_talk::Tick();            // the shopkeeper verdicts queued by a seat's pass
    camera::attack_cameras::Tick();       // the attack-camera status line (Debug)
    camera::attack_framing::Tick();       // the attack-framing latches' clock and status line (Debug)
    camera::seat_camera_effects::Tick();   // the camera-effect status line (Debug)
    battle::synergy_partner::Tick();      // humans' synergy partners: session, late give-back, status (Debug)
    // The code placed from the game thread outside the overlay render (platform/deferred_sites.h): its polls.
    renderer::PollFamilyBoundary();
    renderer::PollMeFeedbackOnce();
    hud::PollMinimapOnce();
    camera::location_rows::Poll();
    camera::location_rows::Tick();
}

void TickAfterSession(const Frame& frame) noexcept
{
    // The per-seat modules loop the built seats themselves; the per-column ones (the framing, the shapes, the
    // renderer's hints) read the frame's column count.
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        g_ownColumns[static_cast<std::size_t>(seat)] = frame.shown.OwnColumn(seat);
    }
    // By seat, whether the seat's player plays a member (index 0 unused): the world modules' per-seat passes. And the
    // pad slots a player holds (a seat's controller id is its seat), whose Start reaches the game.
    std::array<bool, players::kMaxPlayers> plays{};
    std::uint32_t                          playing = 1;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        plays[seat] = frame.seats[seat].PlaysMember();
        playing |= frame.seats[seat].state != players::SeatState::Out ? 1u << seat : 0u;
    }
    input::pad_table::SetPlaying(playing);
    input::walk_limits::Tick();  // after the session's tick: a take's copy is overwritten this frame
    world::nav_strand::Tick(frame.splitShown);  // a stranded player is warped back to the party
    battle::arena_warp::Tick();  // an ask made in a fight: the character brought into the fight's area
    hud::lead_mark::Tick();  // each kept walk's marker added, moved or removed
    hud::low_health::Tick(plays, frame.splitShown);  // each seat's danger decided outside a fight
    camera::camera_repair::Tick();
    traversal::traversal_watch::Tick();
    world::seat_triggers::Tick();
    world::levers::Tick();  // a seat gone: its wishes and in-range marks dropped
    traversal::cart_climb::Tick();  // a seat gone: its cart wishes dropped
    world::seat_switch_volumes::Tick();  // each seat's volumes left once it is gone; the summary
    world::story_boxes::Tick(frame.splitShown);  // the held-back story-box enters: replayed at the story's moment
    traversal::traversal_camera::Tick();  // each seat's idle camera holders; the collision watch (dev)
    camera::near_fade::Tick();         // each seat's character's fade state (Debug, on change)
    input::approach_own_camera::Tick();      // the crossing walk-up calls, swapped once from here
    input::seat_walk_cap::Tick();     // each seat's deferred walk-cap clear, the layer-2 watch
    camera::vault_camera::Tick();      // each seat's vault cameras end on its own camera (lever L2)
    traversal::chocobo_climb::Tick();     // each seat's climb camera's states and the summary (Debug)
    traversal::chocobo_spots::Tick();     // the spots' summary (Debug)
    camera::camera_distance::TickFraming(frame.columns);  // per-layout distance steps (roadmap 2b)
    std::array<float, players::kMaxPlayers> ratios{};
    for (int seat = 0; seat < players::kMaxPlayers; ++seat)
    {
        ratios[static_cast<std::size_t>(seat)] = views::SplitWidthRatio(seat);
    }
    camera::shoulder_scale::Poll(frame.splitShown, ratios);
    world::anim_views::Tick(frame.splitShown);  // its frame count, before the world tick
    world::charapop_near::Tick(frame.splitShown, plays);
    world::charapop_centre::Tick(frame.splitShown, plays);
    world::world_actor_reach::Tick(plays);
    hud::minimap::Tick(frame.splitShown);  // outside the overlay render, before it
    views::UpdateStreamViews(frame.splitShown);  // nothing until whole_screen's game-state check passed
    // Filled views: each main camera's own 16:9 constraint, cleared once by the game's setter, while several views
    // show (a seat's own, or the columns the mirror views fill: a mirror's column on a wider monitor would otherwise
    // keep the game's bars). FillsHalves is false without the view-point entry (nothing shapes the views then), so
    // the cameras keep their constraint.
    camera::main_camera_shape::Update(((players::generation::AnyActive() && frame.splitShown) || frame.viewsSplit) &&
                                      views::FillsHalves());
    renderer::FollowViews(frame.columns > 1);  // the submit hints skipped while several views render
}

namespace {

// Game thread: the console's `view-columns` (0: none), the columns held for the checks on one monitor.
int  g_viewColumns = 0;
int  g_lastWanted  = 1;  // the columns the last columns' tick wanted

#if LSS_DEV
/// The `field.ready` reading, the scripts' readiness: the field running (the entry steps done), the game's battle
/// system and option data built, player 1's camera manager ready for a join; and nothing over the field (a menu, a
/// scene, the title: its scene is a field with a one-member party, which the first three tests accept, and every
/// script then ran at the title). Here because it reads the seats' modules and the views both.
bool FieldReady() noexcept
{
    return session::field_exit::FieldReady() && session::party_member::GameReady() &&
           session::own_controller::Ready() &&
           views::ReasonOf(views::ReadScreenSignals()) == views::WholeScreenReason::None;
}
#endif

/// Why a mirror view may not be added now, or null. A mirror needs no member, no field and nothing clear of the
/// screen (the title, a loading screen and a movie are exactly where it is wanted on the row); player 1's controller
/// and camera manager are checked by the add itself (views/mirror_player.cpp). What holds it is the engine's own
/// refusal (views::LocalPlayerRefused: the pause-family menu), so the add waits for that state to pass instead of
/// failing twice a second.
const wchar_t* MirrorHold() noexcept
{
    return views::LocalPlayerRefused();
}

/// The engine's local player list as columns (views/columns.h): each listed player's controller id in list order.
views::columns::Map ListedColumns() noexcept
{
    std::array<std::int32_t, views::columns::kMaxColumns> ids{};
    std::size_t                                          n      = 0;
    auto*                                                listed = players::unreal::LocalPlayers(players::unreal::GameInstance());
    for (std::int32_t i = 0; listed != nullptr && i < listed->Num() && n < ids.size(); ++i)
    {
        ids[n++] = players::LocalPlayerControllerId((*listed)[i]);
    }
    return views::columns::MapOf(std::span<const std::int32_t>(ids.data(), n));
}

}  // namespace

Columns TickColumns() noexcept
{
    Columns columns;
    const int held    = std::max(views::window_span::HeldColumns(), g_viewColumns);
    const int players = ListedColumns().PlayersListed();
    columns.playersIn = std::max(players, 1);
    columns.held      = held >= 2;
    columns.wanted    = views::columns::Wanted(columns.playersIn, held);
    views::mirror_player::Tick(columns.wanted - columns.playersIn, &MirrorHold);
    columns.map   = ListedColumns();  // with this tick's mirror added or removed
    auto* listed  = players::unreal::LocalPlayers(players::unreal::GameInstance());
    columns.firstPlayer = listed != nullptr && listed->Num() > 0 ? (*listed)[0] : nullptr;
    g_lastColumns = columns.map;
    g_lastWanted  = columns.wanted;
    hud::overlay_pass::SetMap(columns.map);  // the HUD's passes by column, before the HUD's tick and the frame's render
    views::window_span::Tick(columns.wanted);
    return columns;
}

void ClearMirrorsForMapLoad(const wchar_t* why) noexcept
{
    if (!views::mirror_player::Clear(why))
    {
        platform::LogWarning(L"{}: a mirror view could not be removed; the map load would spawn its controller again "
                             L"outside a join (its camera manager's fixed-name spawns are expected to collide)",
                             why);
    }
}

bool SeatTaken(const session::Session& session, int seat) noexcept
{
    return session.In(seat) || session.AwaitingMember(seat) || session.RejoinPending(seat);
}

int SeatToJoin(const session::Session& session) noexcept
{
    // A seat waiting for a member or a rejoin is joined again, as Ctrl+F7 did for player 2 before player 3.
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!session.In(seat))
        {
            return seat;
        }
    }
    return 0;
}

int SeatToLeave(const session::Session& session) noexcept
{
    for (int seat = players::kSeatsBuilt; seat >= 1; --seat)
    {
        if (SeatTaken(session, seat))
        {
            return seat;
        }
    }
    return 0;
}

void LeaveSeat(session::Session& session, int seat, const wchar_t* why) noexcept
{
    platform::LogDebug(L"{}", why);
    session.Leave(seat);
}

Result SetOwnCamera(const session::Session& session, bool on) noexcept
{
    if (!views::ViewPointInstalled())
    {
        return {false, L"the view-point entry is not installed"};
    }
    if (!session.AnyIn())
    {
        return {false, L"no other player is in (Ctrl+F7 adds one)"};
    }
    if (views::ShowsOwnCamera() != on)
    {
        views::ToggleOwnCamera();
    }
    return {true, views::ShowsOwnCamera() ? L"players 2 and 3's own cameras" : L"player 1's camera"};
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

Result SetMonitorMode(bool on) noexcept
{
    if (!views::window_span::Installed())
    {
        return {false, L"the window span is not installed (see the startup log)"};
    }
    if (!on)
    {
        // Off also cancels a try the settings file left waiting for the window (so Held() alone is not the test).
        views::window_span::Hold(false);
        return {true, L"the three-player monitor mode is off"};
    }
    if (views::window_span::Held() && views::window_span::In())
    {
        return {true, L"the three-player monitor mode is already on"};
    }
    if (!views::window_span::Hold(true))
    {
        // The mode stands: with no row now it waits for one (a picked monitor not connected, no row on the desktop).
        return {views::window_span::Held(),
                views::window_span::Held() ? L"the three-player monitor mode is on and waits for its row of monitors (the log says what is missing)"
                                           : L"the three-player monitor mode could not be switched on (the log says why)"};
    }
    if (views::window_span::WaitsForMenu())
    {
        return {true, L"the three-player monitor mode is on once the menu closes"};
    }
    return {true, L"the row of monitors, player 1's picture in every column no player has"};
}

Result SetWindowSpan(const session::Session& session, bool on, const wchar_t* why) noexcept
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
    if (!session.AnyIn())
    {
        return {false, L"no other player is in (Ctrl+F7 first)"};
    }
    // The columns the frame shows (the last columns' tick): one per player in, more while columns are held.
    const auto columns = std::max(g_lastWanted, 2);
    if (!views::window_span::Enter(columns))
    {
        return {false, L"the window could not be spanned (the log says why)"};
    }
    return {true, std::format(L"{} columns", columns)};
}

namespace {

control::Outcome From(Result done)
{
    return {done.ok, std::move(done.text)};
}

}  // namespace

void RegisterOptions(players::SplitLayout& layout) noexcept
{
    // Each owner on its own: one that throws (out of memory) leaves the others' rows working.
    const auto each = [](const wchar_t* owner, auto&& registerOwn) noexcept {
        try
        {
            registerOwn();
        }
        catch (...)
        {
            platform::LogWarning(L"control: registering {}'s options failed (out of memory?)", owner);
        }
    };
    // The three-player monitor mode from its option's row (the settings file's multi-monitor): as Ctrl+F3.
    each(L"the monitor mode", [] {
        control::RegisterOption(
            platform::settings::kMultiMonitor, [](const std::wstring& word) { return From(SetMonitorMode(word == L"on")); },
            [] { return views::window_span::Preset(); });
    });
    each(L"the split layout", [&layout] { players::RegisterOptions(layout); });
    each(L"the column layout", [] { views::column_layout::RegisterOptions(); });
    each(L"the view point", [] { views::RegisterViewPointOptions(); });
    // Every page row's option has its entry now (the rest registered at their owners' installs): a missing one is a row
    // that does nothing, said here in every build.
    for (const platform::settings::Option* option : platform::settings::kOptions)
    {
        if (!option->appOnly && control::Global().Find(option->key) == nullptr)
        {
            platform::LogWarning(L"control: the option '{}' has no entry (its owner did not install, or the registry "
                                 L"refused it): its Co-op page row does nothing",
                                 option->key);
        }
    }
}

#if LSS_DEV

namespace {

/// The commands of the shared actions above, and the field's readiness.
void RegisterOwnCommands(session::Session& session)
{
    using control::Register;
    using control::Shape;
    using control::Value;
    // Each seat's leave (`leave` player 2's, `p3.leave` player 3's); the session registers each seat's join and readings
    // (session::RegisterCommands).
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const std::wstring name   = players::SeatName(seat);
        const std::wstring prefix = std::format(L"p{}", seat + 1);
        Register({.name = players::BySeat(seat, L"leave", L"p3.leave"),
                  .help = std::format(L"remove {} (Ctrl+F8 removes the last player in); `wait-until {}.in == off` "
                                      L"confirms it",
                                      name, prefix),
                  .set  = [&session, seat, name](const Value&) {
                      // As Ctrl+F8: also stops a wait for a member or a rejoin after a map load.
                      if (!SeatTaken(session, seat))
                      {
                          return control::Outcome{false, name + L" is not in"};
                      }
                      LeaveSeat(session, seat, L"a player leaves (the leave command)");  // over the next ticks
                      return control::Outcome{};
                  }});
    }
    Register({.name = L"own-camera", .shape = Shape::OnOff,
              .help = L"players 2 and 3's views show their own cameras (on), or player 1's",
              .set  = [&session](const Value& v) { return From(SetOwnCamera(session, std::get<bool>(v))); },
              .get  = [] { return Value{views::ShowsOwnCamera()}; }});
    Register({.name = L"bars", .shape = Shape::OnOff, .help = L"split views keep 16:9 with bars, or fill their halves",
              .set  = [](const Value& v) { return From(SetFill(!std::get<bool>(v))); },  // bars on: no fill
              .get  = [] { return Value{!views::FillsHalves()}; }});
    Register({.name = platform::settings::kMonitors.key, .shape = Shape::Text, .help = platform::settings::kMonitors.help,
              .get  = [] { return Value{platform::settings::Of(platform::settings::kMonitors).word}; }});
    Register({.name = L"view-columns", .shape = Shape::Integer, .min = 0, .max = views::columns::kMaxColumns,
              .help = L"columns held without the span, for the checks on one monitor: 2 or 3 (the mirror views fill "
                      L"the ones no player has); 0 for none",
              .set  = [](const Value& v) {
                  const auto columns = std::get<std::int64_t>(v);
                  if (columns == 1)
                  {
                      return control::Outcome{false, L"0 (none), 2 or 3"};
                  }
                  g_viewColumns = static_cast<int>(columns);
                  platform::Log(L"view columns: {} held (the console)", g_viewColumns);
                  return control::Outcome{};
              },
              .get = [] { return Value{static_cast<std::int64_t>(g_viewColumns)}; }});
    Register({.name = L"mirror-view.state", .shape = Shape::Text,
              .help = L"the mirror views: wanted, in which columns with which controller ids, why one waits",
              .get  = [] { return Value{views::mirror_player::Reading()}; }});
    Register({.name = L"window-span.state", .shape = Shape::Text,
              .help = L"the window span: out, lining up or in, its columns, the row, the window",
              .get  = [] { return Value{views::window_span::Reading()}; }});
    Register({.name = L"window-span", .shape = Shape::OnOff,
              .help = L"the game window across its row of monitors (the settings file's, moved into a row, or one the "
                      L"desktop has), one column per local player, through the engine's own window reshape; needs "
                      L"player 2 or 3 in",
              .set  = [&session](const Value& v) {
                  return From(SetWindowSpan(session, std::get<bool>(v), L"the window-span command"));
              },
              .get = [] { return Value{views::window_span::In()}; }});
    Register({.name = L"field.ready", .shape = Shape::OnOff,
              .help = L"a save's field is loaded and playable, nothing over it (no menu, scene or title)",
              .get  = [] { return Value{FieldReady()}; }});
}

}  // namespace

void RegisterCommands(session::Session& session, views::column_source::Override& choice) noexcept
{
    try
    {
        // Bottom layer first, so `help` lists the commands from the log level up to the game's modules.
        platform::RegisterLogCommands();
        platform::frame_watch::RegisterCommands();
        players::console::RegisterCommands([&session] { return session.AnyIn(); });
        views::column_source::RegisterCommands(choice);
        session::RegisterCommands(session);
        input::pad_table::RegisterCommands();
        input::pad_presence::RegisterCommands();
        session::player_window::RegisterCommands();
        hud::view_prompt::RegisterCommands();
        hud::view_window::RegisterCommands();
        input::pad_sticks::RegisterCommands();
        scopes::scope_leaks::RegisterCommands();
        input::pad_buttons::RegisterCommands();
        world::nav_strand::RegisterCommands();
        menu::coop_menu::RegisterCommands();
        battle::lock_on::RegisterCommands();
        battle::menu_tint::RegisterCommands();
        hud::low_health::RegisterCommands();
        battle::enemy_leader::RegisterCommands();
        camera::shoulder_scale::RegisterCommands();
        camera::camera_distance::RegisterCommands();
        world::charapop_centre::RegisterCommands();
        world::charapop_near::RegisterCommands();
        views::RegisterViewPointCommands();
        views::RegisterStreamCommands();
        views::column_layout::RegisterCommands();
        hud::RegisterOverlayLayoutCommands();
        hud::ui_effect_pass::RegisterCommands();
        hud::RegisterColumnHudImagesCommands();
        hud::RegisterSeatHudContentCommands();
        hud::column_area_icons::RegisterCommands();
        hud::RegisterMinimapOnceCommands();
        hud::RegisterHudTraceCommands();
        hud::RegisterPositionProbeCommands();
        hud::RegisterDumpCommands();
        renderer::RegisterMeFeedbackCommands();
        renderer::RegisterSubmitHintCommands();
        renderer::RegisterSubmissionCensusCommands();
        renderer::RegisterViewFamilyCommands();
        RegisterOwnCommands(session);
    }
    catch (...)
    {
        platform::Log(L"control: registering the commands failed (out of memory?); the table is incomplete");
    }
}

#endif

}  // namespace lss::modules
