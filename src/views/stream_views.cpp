#include "views/stream_views.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "views/player_view_point.h"
#include "views/stream_view_math.h"
#include "views/whole_screen.h"

#include <cstdint>
#include <initializer_list>

namespace lss::views {

namespace {

// End_AddViewSlaveLocation (RE-p2-distant-lod-swap.md, "Slave-location call facts"; this build).
constexpr std::uintptr_t kAddViewSlaveLocation      = 0x140A97DE0;
constexpr std::uint8_t   kAddViewSlaveLocationBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x70,
                                                         0x00, 0x00, 0x00, 0xE8, 0x40, 0x71, 0x61, 0x01};

using AddViewSlaveLocationFn = char (*)(const float* location, const float* direction, float fovRadians,
                                        float mainPassScale, bool skipSlaveFrusta, float slaveFar, float shadowFar);

constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;
constexpr float kMainPassScale    = 1.0f;   // no change to the main pass's frustum scale
constexpr bool  kSkipSlaveFrusta  = false;  // true would switch the slave frustums off until the game restarts
constexpr float kSlaveFar         = 0.0f;   // unbounded, as the rail camera
constexpr float kShadowFar        = 0.0f;   // no static-shadow streaming around the camera

// Set once at install.
AddViewSlaveLocationFn g_add       = nullptr;
bool                   g_installed = false;

// Game thread (the control channel that sets the mode also runs there).
StreamViewsMode g_mode       = StreamViewsMode::Both;
StreamViewsMode g_loggedMode  = StreamViewsMode::Off;
bool            g_active      = false;  // cameras were registered on the last update
bool            g_failed      = false;  // the game's function raised an exception: never called again

const wchar_t* NameOf(StreamViewsMode mode) noexcept
{
    switch (mode)
    {
        case StreamViewsMode::Off: return L"off";
        case StreamViewsMode::PlayerTwo: return L"player 2's camera";
        case StreamViewsMode::Both: return L"both players' cameras";
    }
    return L"?";
}

/// Registers one camera; false if the game's function raised an exception.
bool Register(const StreamCamera& camera, float aspect) noexcept
{
    const float fov = CoveringFov16x9(camera.fov, aspect) * kRadiansPerDegree;
    char        result = 0;  // whether the game added the camera (it merges one it already has); not needed here
    return platform::CallGuarded(g_add, result, camera.location, camera.direction, fov, kMainPassScale,
                                 kSkipSlaveFrusta, kSlaveFar, kShadowFar);
}

}  // namespace

bool InstallStreamViews(const platform::GameImage& image, bool viewPointInstalled) noexcept
{
    if (!viewPointInstalled)
    {
        platform::Log(L"streaming views: off (the view point hook that records each player's drawn camera is not "
                      L"installed); each view's scenery detail stays as the game's shared requests leave it");
        return false;
    }
    const auto add = image.Rebase(kAddViewSlaveLocation);
    if (!add || !image.BytesMatch(kAddViewSlaveLocation, kAddViewSlaveLocationBytes))
    {
        platform::Log(L"streaming views: the game's slave-location function is not what the research read; each "
                      L"view's scenery detail stays as the game's shared requests leave it");
        return false;
    }
    g_add       = reinterpret_cast<AddViewSlaveLocationFn>(*add);
    g_installed = true;
    return true;
}

bool StreamViewsInstalled() noexcept
{
    return g_installed;
}

void SetStreamViewsMode(StreamViewsMode mode) noexcept
{
    g_mode = mode;
}

StreamViewsMode GetStreamViewsMode() noexcept
{
    return g_mode;
}

void UpdateStreamViews(bool splitShown) noexcept
{
    const bool wanted = g_installed && !g_failed && splitShown && g_mode != StreamViewsMode::Off && GameStatePresent();
    if (wanted != g_active || (wanted && g_mode != g_loggedMode))
    {
        g_loggedMode = wanted ? g_mode : StreamViewsMode::Off;
        platform::Log(L"streaming views: {}", wanted ? NameOf(g_mode)
                                                     : L"none registered (the game's own views only)");
    }
    g_active = wanted;
    if (!wanted)
    {
        return;
    }
    const float aspect = HalfAspect();
    for (const bool playerTwo : {true, false})
    {
        if (!playerTwo && g_mode != StreamViewsMode::Both)
        {
            continue;
        }
        const auto camera = RenderedCamera(playerTwo);
        if (camera && !Register(*camera, aspect))
        {
            g_failed = true;
            g_active = false;
            platform::Log(L"streaming views: the game's slave-location function raised an exception; not called again");
            return;
        }
    }
}

}  // namespace lss::views
