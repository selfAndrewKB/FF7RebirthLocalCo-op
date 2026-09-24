#include "players/join_keys.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players {

namespace {

bool IsDown(int virtualKey) noexcept
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool GameHasFocus() noexcept
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

/// True on the tick the key goes down; `wasDown` carries the state between ticks.
bool Pressed(bool isDown, bool& wasDown) noexcept
{
    const bool pressed = isDown && !wasDown;
    wasDown            = isDown;
    return pressed;
}

}  // namespace

PlayerIntent JoinKeys::Poll() noexcept
{
    const bool active = GameHasFocus() && IsDown(VK_CONTROL);
    const bool join   = Pressed(active && IsDown(VK_F7), joinWasDown_);
    const bool leave  = Pressed(active && IsDown(VK_F8), leaveWasDown_);
    const bool turn   = Pressed(active && IsDown(VK_F6), turnWasDown_);
    const bool layout = Pressed(active && IsDown(VK_F5), layoutWasDown_);
    const bool fill   = Pressed(active && IsDown(VK_F4), fillWasDown_);

    // Joining and leaving win over everything else pressed on the same tick; of the switches,
    // one per tick (the others are reported on their own presses).
    if (!join && !leave)
    {
        if (turn)
        {
            return PlayerIntent::ToggleTestTurn;
        }
        if (layout)
        {
            return PlayerIntent::ToggleLayout;
        }
        if (fill)
        {
            return PlayerIntent::ToggleFill;
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

}  // namespace lss::players
