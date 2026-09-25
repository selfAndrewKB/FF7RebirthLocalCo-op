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
//   hud-levers-mask <0..255>             diagnosis: which levers act (1 contexts, 2 first player,
//                                         4 primary controller, 8 view rect, 16 updates, 32 compass,
//                                         64 the arrows' own timers, 128 the enemy bars per view)
//   sticks-mask <0..7>                   diagnosis: which parts of per-pad sticks act (1 player 2's
//                                         input as the primary player's, 2 player 1's move state on
//                                         pad 1, 4 player 2's member ticked); all at start
//   leak-fixes <0..7>                    diagnosis: which put-backs of player 2's scope leaks act (1 the
//                                         input clock, 2 the leader transform, 4 field summons skipped;
//                                         player_two/scope_leaks.h); all at start
//   buttons-mask <0..3>                  diagnosis: which parts of per-pad buttons act (1 the split views,
//                                         2 pad 2 kept from menus and prompts); both at start
//   icons-mask <0..7>                    diagnosis: which parts of player 2's own map icons act (1 its
//                                         icon manager ticked, 2 its first controller player 2's, 4 each
//                                         half shows its own canvases; hud/area_icons_two.h); all at start
//   perf-log on|off                      each hitch's own line and the periodic timing lines (off at
//                                         start; the 10 s hitch summary is always written)
//   hud-trace <frames>                  log the order of the HUD's tick, marker placements, icon
//                                         projections and render for 1-30 frames, and which player
//                                         each placement and projection used (hud/hud_trace.h)
//   dump widgets                          log each visible HUD widget's desired size and geometry
//                                         (hud/widget_dump.h)
//   dump slots <Class>                    log a widget class's viewport layout and inner slots
//   dump hud-players                      log which player the HUD roots and its global player
//                                         reads name (hud/hud_players_dump.h)
//   stick <player> <lx> <ly> <rx> <ry> <ticks>
//                                         hold a player's sticks (-1..1) for a number of ticks;
//                                         needs per-pad sticks, and wins over the pad itself
//   wait <ticks>                          do nothing for a number of ticks
//   wait-until playing|out [<ticks>]      until player 2 plays their member, or is out
//                                         (fails after the timeout, 600 ticks by default)
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
    PerfLog,
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
};

struct Command
{
    CommandKind   kind      = CommandKind::Note;
    bool          on        = false;                   // own-camera on / sticks per-pad / layout side-by-side / bars on
    std::int32_t  player    = 0;                       // stick: 1 or 2
    float         axes[4]   = {};                      // stick: LX, LY, RX, RY
    float         blend     = -1.0f;                   // split-fov: 0..1, or negative for native
    std::uint32_t ticks     = 0;                       // stick, wait, wait-until; hud-trace: frames; the masks: mask
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
