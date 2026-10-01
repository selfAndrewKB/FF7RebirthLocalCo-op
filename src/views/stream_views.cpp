#include "views/stream_views.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/unreal_access.h"
#include "views/player_view_point.h"
#include "views/stream_view_math.h"
#include "views/whole_screen.h"

#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>

namespace lss::views {

namespace {

// End_AddViewSlaveLocation (RE-p2-distant-lod-swap.md, "Slave-location call facts"; this build).
constexpr std::uintptr_t kAddViewSlaveLocation      = 0x140A97DE0;
constexpr std::uint8_t   kAddViewSlaveLocationBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x70,
                                                         0x00, 0x00, 0x00, 0xE8, 0x40, 0x71, 0x61, 0x01};

using AddViewSlaveLocationFn = char (*)(const float* location, const float* direction, float fovRadians,
                                        float mainPassScale, bool skipSlaveFrusta, float slaveFar, float shadowFar);

constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;
// 0 keeps r.MassiveEnvironment.FrustumScaleForMainPass as the game set it. A positive value is written to that cvar
// every tick (EndGameState tick 0x140C94690), so 1.0 would halve the game's off-screen margin (2 on Windows) for good.
constexpr float kMainPassScale    = 0.0f;
constexpr const wchar_t* kMainPassMarginVariable = L"r.MassiveEnvironment.FrustumScaleForMainPass";
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
float           g_playerOneFov = 0.0f;  // degrees player 1's camera is registered with; 0 = what its half covers
float           g_loggedFov    = -1.0f;

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
bool Register(const StreamCamera& camera, float aspect, float wantedFov) noexcept
{
    const float fov = StreamFov(CoveringFov16x9(camera.fov, aspect), wantedFov) * kRadiansPerDegree;
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

void SetPlayerOneStreamFov(float degrees) noexcept
{
    g_playerOneFov = StreamFov(0.0f, degrees);
}

void UpdateStreamViews(bool splitShown) noexcept
{
    const bool wanted = g_installed && !g_failed && splitShown && g_mode != StreamViewsMode::Off && GameStatePresent();
    const bool fovChanged = g_mode == StreamViewsMode::Both && g_playerOneFov != g_loggedFov;  // it applies under both only
    if (wanted != g_active || (wanted && (g_mode != g_loggedMode || fovChanged)))
    {
        g_loggedMode = wanted ? g_mode : StreamViewsMode::Off;
        g_loggedFov  = g_playerOneFov;
        // The mod never writes the margin (kMainPassScale 0), so this is the game's own value at every line.
        const auto   margin = players::unreal::ConsoleVariableFloat(kMainPassMarginVariable);
        std::wstring marginText;
        try
        {
            marginText = margin ? std::format(L"{:.2f}", *margin) : std::wstring(L"unread");
        }
        catch (...)
        {
        }
        if (!wanted)
        {
            platform::Log(L"streaming views: none registered (the game's own views only); the game's off-screen margin {}",
                          marginText);
        }
        else if (g_mode == StreamViewsMode::Both && g_playerOneFov > 0.0f)
        {
            platform::Log(L"streaming views: {}; player 1's registered {:.0f} degrees wide; the game's off-screen margin {}",
                          NameOf(g_mode), g_playerOneFov, marginText);
        }
        else
        {
            platform::Log(L"streaming views: {}; the game's off-screen margin {}", NameOf(g_mode), marginText);
        }
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
        if (camera && !Register(*camera, aspect, playerTwo ? 0.0f : g_playerOneFov))
        {
            g_failed = true;
            g_active = false;
            platform::Log(L"streaming views: the game's slave-location function raised an exception; not called again");
            return;
        }
    }
}

}  // namespace lss::views
