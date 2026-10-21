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
    const bool camera = Pressed(active && IsDown(VK_F6), cameraWasDown_);
    const bool layout = Pressed(active && IsDown(VK_F5), layoutWasDown_);
    const bool fill   = Pressed(active && IsDown(VK_F4), fillWasDown_);
    const bool sticks = Pressed(active && IsDown(VK_F9), sticksWasDown_);
    const bool centre = Pressed(active && IsDown(VK_F10), centreWasDown_);
    const bool detail = Pressed(active && IsDown(VK_F11), detailWasDown_);
    const bool third  = Pressed(active && IsDown(VK_F3), thirdWasDown_);
    const bool span   = Pressed(active && IsDown(VK_F2), spanWasDown_);

    return ResolveIntent(join, leave, camera, layout, fill, sticks, centre, detail, third, span);
}

}  // namespace lss::players
