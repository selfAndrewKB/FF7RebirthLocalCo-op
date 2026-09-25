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
//   hud-probe on|off                      log how HUD widgets place themselves from Blueprints
//                                         (hud/position_probe.h)
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
    std::uint32_t ticks     = 0;                       // stick, wait, wait-until
    WaitCondition condition = WaitCondition::Playing;  // wait-until
    std::wstring  text;                                // console, note
};

/// The longest a stick hold, wait or wait-until may last: 10 minutes at 60 ticks per second.
inline constexpr std::uint32_t kMaxTicks = 36'000;

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
