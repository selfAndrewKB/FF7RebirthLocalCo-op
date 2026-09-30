#include "dev/dev_tools.h"

#include "control/command_channel.h"
#include "control/mod_target.h"
#include "dev/terrain_watch.h"
#include "hud/hud_trace.h"
#include "hud/position_probe.h"

#include <optional>

namespace lss::dev {

namespace {

// Game thread only.
std::optional<control::ModTarget> g_target;
control::CommandChannel           g_channel;

}  // namespace

void Install(const platform::GameImage& image, player_two::Session& session, players::SplitLayout& layout,
             players::SingleView& singleView, bool viewPointInstalled) noexcept
{
    hud::InstallPositionProbe(image);  // logging only, off until the hud-probe command
    hud::InstallHudTrace(image);       // logging only, idle until the hud-trace command
    terrain_watch::Install(image);     // logging only, while player 2 is in
    InstallControl(session, layout, singleView, viewPointInstalled);
}

void InstallControl(player_two::Session& session, players::SplitLayout& layout, players::SingleView& singleView,
                    bool viewPointInstalled) noexcept
{
    // Control scripts drive the same actions as the keys, from files (control/command_channel.h).
    g_target.emplace(session, layout, singleView, viewPointInstalled);
    g_channel.Open();
}

void Remove() noexcept
{
    hud::RemovePositionProbe();
    hud::RemoveHudTrace();
}

void TickBeforeSession() noexcept
{
    if (g_target)
    {
        g_channel.Tick(*g_target);
    }
    terrain_watch::Tick();
}

void TickAfterSession() noexcept
{
    if (g_target)
    {
        g_target->DropEndedHolds();
    }
}

void BeforeOverlayRender() noexcept
{
    hud::TraceBeforeRender();
}

void AfterOverlayRender() noexcept
{
    hud::TraceAfterRender();
}

}  // namespace lss::dev
