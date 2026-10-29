#include "dev/dev_tools.h"

#include "battle/battle_input.h"
#include "battle/menu_focus.h"
#include "battle/target_census.h"
#include "camera/framing_watch.h"
#include "control/remote.h"
#include "control/stick_holds.h"
#include "feedback/rumble_watch.h"
#include "hud/hud_trace.h"
#include "hud/minimap/label_relayouts.h"
#include "hud/position_probe.h"
#include "input/move_census.h"
#include "platform/frame_watch.h"
#include "platform/log.h"
#include "renderer/submission_census.h"
#include "rides/ride_watch.h"
#include "rides/wheelie_watch.h"
#include "session/handover_watch.h"
#include "session/tag_counts.h"
#include "world/charapop_watch.h"
#include "world/interact_watch.h"

#include <optional>

namespace lss::dev {

namespace {

// Game thread, but for Remove (the mod's destructor, which may run off it).
std::optional<control::StickHolds> g_holds;
// Never destroyed: a console still open at process exit (player 2 in, so Remove never ran) would otherwise close in
// DllMain, under the loader lock, taking a mutex a killed thread may hold and calling WSACleanup there.
control::Remote& g_remote = *new control::Remote;

}  // namespace

void Install(const platform::GameImage& image, session::Session& session) noexcept
{
    // The HUD trace and the position probe are installed by their first command, not here:
    // their call redirects sit on the HUD's hot path and nearly every dev start never asks for them.
    // The watches: each reads and logs, changes nothing, and reports its own install (its header says what it reads).
    feedback::rumble_watch::Install(image);    // each controller's rumble, pad by pad
    rides::ride_watch::Install(image);      // the chocobo rides and the vehicle coordinators
    rides::wheelie_watch::Install(image);   // the Wheelie system
    camera::framing_watch::Install(image);   // each player's field-camera framing
    world::charapop_watch::Install(image);  // the character detail near player 2
    session::handover_watch::Install(image);  // the party order and the members' AI at hand-overs
    battle::menu_focus::Install(image);      // player 1's UI focus, on change
    battle::battle_input::Install(image);    // player 2's battle state inside the member tick, on change (Trace)
    input::move_census::Install(image);     // the move-state ticks and gate tests per character (the status line)
    battle::target_census::Install(image);   // whom enemies target, and by which stage (after the targeting group)
    world::interact_watch::Install(image);  // the field interactions around player 2's pickups (after field_pickup)
    const bool tags = session::tag_counts::Install(image);  // the actor-tag registry (the session's Debug counts)
    platform::Log(L"tag counts: {}", tags ? L"readable (the session's take and leave counts, Debug)"
                                          : L"NOT readable (a check failed); the session's counts are not logged");
    hud::label_relayouts::Install(image);        // which text blocks relayout, where and why (the minimap marks the scopes)
    renderer::InstallSubmissionCensusIfAsked();  // the GPU submission census: census.txt at launch, or `census on`
    platform::frame_watch::AddWindowReport([](std::uint32_t, double, bool log) {
        if (log)
        {
            renderer::LogSubmissionCensus();  // the census's summary of the perf log's window
        }
    });
    InstallControl(session);
}

void InstallControl(session::Session& session) noexcept
{
    // The console drives the same actions as the keys (control/remote.h). The commands are registered by then
    // (mod.cpp: modules::RegisterCommands, or the console's alone in the watch-only start); the console logs the
    // registry's refusals as it opens.
    g_holds.emplace(session);
    g_remote.Open(*g_holds);
}

void Remove() noexcept
{
    g_remote.Close();  // the last installed: its socket thread joined first
    hud::RemovePositionProbe();  // each a no-op unless a command installed it
    hud::RemoveHudTrace();
    hud::label_relayouts::Remove();
    world::interact_watch::Remove();  // its entries back; the pass tells no one
    battle::menu_focus::Remove();
    battle::target_census::Remove();  // its observers unregistered; its own hooks pass through
    renderer::RemoveSubmissionCensus();
    feedback::rumble_watch::Remove();  // the first installed, the last out
}

void TickBeforeSession() noexcept
{
    g_remote.Tick();  // nothing while closed
}

void TickAfterSession() noexcept
{
    if (g_holds)
    {
        g_holds->DropEndedHolds();
    }
}

void TickWatches(bool splitShown) noexcept
{
    rides::ride_watch::Tick();
    rides::wheelie_watch::Tick();
    camera::framing_watch::Tick(splitShown);
    world::charapop_watch::Tick(splitShown);
    session::handover_watch::Tick();
}

}  // namespace lss::dev
