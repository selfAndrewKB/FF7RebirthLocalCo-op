#pragma once

// The keys that express player intent:
//   Ctrl+F7  add player 2            Ctrl+F8  remove player 2
//   Ctrl+F5  top/bottom <-> side by side
//   Ctrl+F4  halves that fill <-> halves kept 16:9 with bars
//   Ctrl+F6  player 2's half: player 2's own camera (at join) <-> player 1's camera (testing)
//   Ctrl+F9  each pad drives only its own player's sticks (on at start; off merges them, for testing)
//   Ctrl+F10 the NPC population follows both players (on at start; off follows the leader, to compare)
//   Ctrl+F11 characters near player 2 keep full detail (on at start; off: player 1's camera alone, to compare)
//   Ctrl+F3  the three-player monitor mode on and off (three monitors, player 1's picture in the empty columns;
//            views/window_span.h)
//   Ctrl+F2  the game window across its row of monitors and back (views/window_span.h)
// Keys never reach renderer internals; the renderer follows the player count.

namespace lss::players {

enum class PlayerIntent
{
    None,
    Join,
    Leave,
    ToggleLayout,
    ToggleFill,
    ToggleOwnCamera,
    TogglePadSticks,
    ToggleNpcCentre,
    ToggleNpcDetail,
    ToggleMonitorMode,
    ToggleWindowSpan,
};

/// The intent of one tick's presses: joining and leaving win over everything else pressed on the same tick (and
/// cancel each other); of the switches, one per tick, in this order (the others are reported on their own presses).
constexpr PlayerIntent ResolveIntent(bool join, bool leave, bool camera, bool layout, bool fill, bool sticks, bool centre,
                                     bool detail, bool monitors, bool span) noexcept
{
    if (!join && !leave)
    {
        if (camera)
        {
            return PlayerIntent::ToggleOwnCamera;
        }
        if (layout)
        {
            return PlayerIntent::ToggleLayout;
        }
        if (fill)
        {
            return PlayerIntent::ToggleFill;
        }
        if (sticks)
        {
            return PlayerIntent::TogglePadSticks;
        }
        if (centre)
        {
            return PlayerIntent::ToggleNpcCentre;
        }
        if (detail)
        {
            return PlayerIntent::ToggleNpcDetail;
        }
        if (monitors)
        {
            return PlayerIntent::ToggleMonitorMode;
        }
        if (span)
        {
            return PlayerIntent::ToggleWindowSpan;
        }
    }
    if (join && !leave)
    {
        return PlayerIntent::Join;
    }
    if (leave && !join)
    {
        return PlayerIntent::Leave;
    }
    return PlayerIntent::None;
}

class JoinKeys
{
public:
    /// Reads the keys once per tick. Reports an intent on the press, not while held, and only
    /// while the game window has focus.
    PlayerIntent Poll() noexcept;

private:
    bool joinWasDown_   = false;
    bool leaveWasDown_  = false;
    bool cameraWasDown_ = false;
    bool layoutWasDown_ = false;
    bool fillWasDown_   = false;
    bool sticksWasDown_ = false;
    bool centreWasDown_ = false;
    bool detailWasDown_ = false;
    bool monitorsWasDown_  = false;
    bool spanWasDown_   = false;
};

}  // namespace lss::players
