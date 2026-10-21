#pragma once

// The control script language: one command per line, read by the command channel
// (control/command_channel.h). Blank lines and lines starting with '#' are skipped.
//
//   join | leave                          add or remove player 2 (as Ctrl+F7 / Ctrl+F8)
//   own-camera on|off                     player 2's half shows their own camera, or player 1's
//   sticks per-pad|merged                 each pad drives only its own player, or the game's merge
//   layout top-bottom|side-by-side        how the screen is split (now, or from the next join)
//   bars on|off                           split views keep 16:9 with bars, or fill their halves
//   split-fov <weight>|native             filled halves' FOV: 0 keeps the single-player framing,
//                                         1 its on-screen scale (views/view_shape.h, SplitFov;
//                                         0.5 at start); native = the stock MajorAxisFOV rule
//   hud-band on|off                       lay the HUD out in the part of its image each half shows
//                                         (hud/overlay_layout.h); off logs the values only
//   hud-image on|off                     player 2's HUD image: the overlay rendered a second time
//                                         and drawn only in player 2's half (hud/second_image.h)
//   hud-levers on|off                    player 2's image shows player 2's content: the game's
//                                         player levers switched around player 2's pass
//                                         (hud/player_two_pass.h)
//   hud-levers-mask <0..511>             diagnosis: which levers act (1 contexts, 2 first player,
//                                         4 primary controller, 8 view rect, 16 updates, 32 compass,
//                                         64 the arrows' own timers, 128 the enemy gauges per view,
//                                         256 the battle popups per view)
//   sticks-mask <0..15>                  diagnosis: which parts of per-pad sticks act (1 player 2's
//                                         input as the primary player's, 2 player 1's move state on
//                                         pad 1, 4 player 2's member ticked, 8 player 1's input update
//                                         and leader-action tick on pad 1); all at start
//   leak-fixes <0..7>                    diagnosis: which put-backs of player 2's scope leaks act (1 the
//                                         input clock, 2 the leader transform, 4 field summons skipped;
//                                         player_two/scope_leaks.h); all at start
//   buttons-mask <0..3>                  diagnosis: which parts of per-pad buttons act (1 the split views,
//                                         2 pad 2 kept from menus and prompts); both at start
//   icons-mask <0..15>                   diagnosis: which parts of player 2's own map icons act (1 its
//                                         icon manager ticked, 2 its first controller player 2's, 4 each
//                                         half shows its own canvases, 8 ranged from player 2's member;
//                                         hud/area_icons_two.h); all at start
//   battle-mask <0..511>                 diagnosis: bit 8 runs player 2's targeting (the game's
//                                         targeting tick for player 2; player_two/lock_on.h); the other
//                                         bits are retired and ignored; on at start
//   log-level error|warning|info|debug|trace
//                                         the most detailed log level written (platform/log.h); info at
//                                         start, debug adds per-event diagnostics, trace per-frame and
//                                         per-input reports
//   perf-log on|off                      each hitch's own line, the 10 s hitch summary and the periodic
//                                         timing lines (off at start)
//   stream-views off|p2|both             which split players' cameras are registered with the game's own
//                                         scenery streaming (views/stream_views.h); both at start
//   stream-fov-p1 auto|<degrees>         the horizontal FOV (1-170) player 1's camera is registered with
//                                         under both, or auto for what its half covers (at start)
//   stream-feedback p1|each              which view families hand the game's scenery streaming their detail
//                                         requests: player 1's only, as with one player (p1, at start), or
//                                         each family (renderer/me_feedback_once.h)
//   submit-hints skip|stock              parallel passes without their own GPU submission (skip), or the game's
//                                         (renderer/submit_hints.h); skip while player 2 is in, set again at the
//                                         next join or leave
//   census on|off                        diagnosis: the GPU submission census in the perf log's window
//                                         (renderer/submission_census.h); off at start (census.txt: on)
//   feedback-mask <0..31>                diagnosis: which of player 2's skips under stream-feedback p1 act (1 the
//                                         readback processing, 2 the main pass's copy, 4 the hidden-component
//                                         pass, 8 the phase 8 mip-request dispatches, 16 the shadow pass's
//                                         streaming writes); all at start
//   family-split on|off                  diagnosis: one view family per player (on, at start), or both views in
//                                         the engine's one family (off; breaks the multi-view rules, so only
//                                         with own-camera off; renderer/view_family_submission.h)
//   minimap-once on|off                  the minimap's map is rendered once per frame, by player 1's HUD
//                                         pass (on, at start), or again in player 2's (hud/minimap_once.h)
//   shoulder-scale on|off                 each half's camera shoulder offset scaled to its width (on at start;
//                                         player_two/shoulder_scale.h)
//   shoulder-scale-min <0.25..1>          the smallest shoulder factor (0.7 at start; side by side's width alone
//                                         gives 0.5)
//   framing-offset <tb> <sbs>            field-camera distance steps added to the Options level while player 2
//                                         is in, per layout (-2..5 each; 0 and 0 at start;
//                                         player_two/camera_distance.h)
//   npc-centre on|off                    the NPC population follows both players while split (on at start),
//                                         or the leader as the game alone does (player_two/charapop_centre.h)
//   npc-detail full|reduced|off          characters near player 2 that player 1's camera does not keep get full
//                                         detail within the game's distances from player 2's camera (full, at
//                                         start; on means full), within half of them (reduced), or only what
//                                         player 1's camera gives them (off; player_two/charapop_near.h)
//   single-view auto|on|off              player 1's single view while player 2 is in: for the game's
//                                         whole-screen states (auto, the default), always, or never
//                                         (players/single_view.h)
//   third-view on|off                    a third full-size view (a third local player on player 1's camera,
//                                         three columns) for the multi-monitor measurement; needs player 2 in
//                                         (views/third_view.h)
//   window-span on|off                   the game window across its row of equal monitors, one column per local
//                                         player, through the engine's own window reshape; needs player 2 in
//                                         (views/window_span.h)
//   hud-trace <frames>                  log the order of the HUD's tick, marker placements, icon
//                                         projections and render for 1-30 frames, and which player
//                                         each placement and projection used (hud/hud_trace.h)
//   dump widgets                          log each visible HUD widget's desired size and geometry
//                                         (hud/widget_dump.h)
//   dump slots <Class>                    log a widget class's viewport layout and inner slots
//   dump hud-players                      log which player the HUD roots and its global player
//                                         reads name (hud/widget_dump.h)
//   stick <player> <lx> <ly> <rx> <ry> <ticks>
//                                         hold a player's sticks (-1..1) for a number of ticks;
//                                         needs per-pad sticks, and wins over the pad itself
//   wait <ticks>                          do nothing for a number of ticks
//   wait-until playing|out|field [<ticks>]
//                                         until player 2 plays their member, or is out, or the field is loaded
//                                         and playable (fails after the timeout, 600 ticks by default)
//   console <command>                     an engine console command, as typed in the console
//   quit                                  quit the game through the engine (player 2 must be out)
//   status                                write player 2's status line to the log
//   note <text>                           write a marker line to the log
//
// Parsing is pure, so it is tested on the host (tests/control_script_test.cpp).

#include "hud/hud_trace.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lss::control {

enum class CommandKind
{
    Join,
    Leave,
    OwnCamera,
    Sticks,
    Layout,
    Bars,
    SplitFov,
    HudBand,
    HudProbe,
    HudTrace,
    HudImage,
    HudLevers,
    HudLeversMask,
    SticksMask,
    LeakFixes,
    ButtonsMask,
    IconsMask,
    BattleMask,
    PerfLog,
    LogLevel,
    SingleView,
    ThirdView,
    WindowSpan,
    StreamViews,
    StreamFovPlayerOne,
    StreamFeedback,
    SubmitHints,
    Census,
    FeedbackMask,
    MinimapOnce,
    FamilySplit,
    FramingOffset,
    ShoulderScale,
    ShoulderScaleMin,
    NpcCentre,
    NpcDetail,
    Dump,
    Stick,
    Wait,
    WaitUntil,
    Console,
    Quit,
    Status,
    Note,
};

enum class WaitCondition
{
    Playing,
    Out,
    Field,  // the field runs, the game is ready and player 1's controller is ready: a save loaded and playable
};

struct Command
{
    CommandKind   kind      = CommandKind::Note;
    bool          on        = false;                   // own-camera on / sticks per-pad / layout side-by-side / bars on
    std::int32_t  player    = 0;                       // stick: 1 or 2
    float         axes[4]   = {};                      // stick: LX, LY, RX, RY
    float         blend     = -1.0f;                   // split-fov: 0..1, or negative for native; shoulder-scale-min
    std::uint32_t ticks     = 0;                       // stick, wait, wait-until; hud-trace: frames; the masks: mask;
                                                       // single-view: 0 auto, 1 on, 2 off;
                                                       // stream-views: 0 off, 1 p2, 2 both;
                                                       // npc-detail: 0 off, 1 reduced, 2 full;
                                                       // stream-fov-p1: degrees, 0 auto;
                                                       // log-level: 0 error .. 4 trace
    std::int32_t  offsets[2] = {};                     // framing-offset: top/bottom, side by side
    WaitCondition condition = WaitCondition::Playing;  // wait-until
    std::wstring  text;                                // console, note
};

/// The longest a stick hold, wait or wait-until may last: 10 minutes at 60 ticks per second.
inline constexpr std::uint32_t kMaxTicks = 36'000;

/// The most frames one hud-trace logs.
inline constexpr std::uint32_t kMaxTraceFrames = static_cast<std::uint32_t>(hud::kMaxHudTraceFrames);

/// A wait-until's timeout when the script gives none.
inline constexpr std::uint32_t kDefaultWaitUntilTicks = 600;

struct ParsedLine
{
    std::optional<Command> command;  // empty for a blank line or a comment, or on an error
    std::wstring           error;    // set when the line is not a valid command
};

ParsedLine ParseLine(std::wstring_view line);

/// The command as the script would write it, for logs and results.
std::wstring Describe(const Command& command);

}  // namespace lss::control
