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
