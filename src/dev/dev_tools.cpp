#include "dev/dev_tools.h"

#include "control/command_channel.h"
#include "control/stick_holds.h"
#include "hud/hud_trace.h"
#include "hud/minimap/label_relayouts.h"
#include "hud/position_probe.h"
#include "platform/frame_watch.h"
#include "platform/log.h"
#include "player_two/battle/battle_input.h"
#include "player_two/battle/menu_focus.h"
#include "player_two/battle/target_census.h"
#include "player_two/camera/framing_watch.h"
#include "player_two/feedback/rumble_watch.h"
#include "player_two/input/move_census.h"
#include "player_two/rides/ride_watch.h"
#include "player_two/rides/wheelie_watch.h"
#include "player_two/session/handover_watch.h"
#include "player_two/session/tag_counts.h"
#include "player_two/world/charapop_watch.h"
#include "player_two/world/interact_watch.h"
#include "renderer/submission_census.h"

#include <optional>

namespace lss::dev {

namespace {

// Game thread only.
std::optional<control::StickHolds> g_holds;
control::CommandChannel            g_channel;

}  // namespace

void Install(const platform::GameImage& image, player_two::Session& session) noexcept
{
    // The HUD trace and the position probe are installed by their first command (control/mod_commands.cpp), not here:
    // their call redirects sit on the HUD's hot path and nearly every dev start never asks for them.
    // The watches: each reads and logs, changes nothing, and reports its own install (its header says what it reads).
    player_two::rumble_watch::Install(image);    // each controller's rumble, pad by pad
    player_two::ride_watch::Install(image);      // the chocobo rides and the vehicle coordinators
    player_two::wheelie_watch::Install(image);   // the Wheelie system
    player_two::framing_watch::Install(image);   // each player's field-camera framing
    player_two::charapop_watch::Install(image);  // the character detail near player 2
    player_two::handover_watch::Install(image);  // the party order and the members' AI at hand-overs
    player_two::menu_focus::Install(image);      // player 1's UI focus, on change
    player_two::battle_input::Install(image);    // player 2's battle state inside the member tick, on change (Trace)
    player_two::move_census::Install(image);     // the move-state ticks and gate tests per character (the status line)
    player_two::target_census::Install(image);   // whom enemies target, and by which stage (after the targeting group)
    player_two::interact_watch::Install(image);  // the field interactions around player 2's pickups (after field_pickup)
    const bool tags = player_two::tag_counts::Install(image);  // the actor-tag registry (the session's Debug counts)
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

void InstallControl(player_two::Session& session) noexcept
{
    // Control scripts drive the same actions as the keys, from files (control/command_channel.h). The commands are
    // registered by then (mod.cpp: modules::RegisterCommands, or the console's alone in the watch-only start); the
    // channel logs the registry's refusals as it opens.
    g_holds.emplace(session);
    g_channel.Open(*g_holds);
}

void Remove() noexcept
{
    hud::RemovePositionProbe();  // each a no-op unless a command installed it
    hud::RemoveHudTrace();
    hud::label_relayouts::Remove();
    player_two::interact_watch::Remove();  // its entries back; the pass tells no one
    player_two::menu_focus::Remove();
    player_two::target_census::Remove();  // its observers unregistered; its own hooks pass through
    renderer::RemoveSubmissionCensus();
    player_two::rumble_watch::Remove();  // the first installed, the last out
}

void TickBeforeSession() noexcept
{
    g_channel.Tick();  // nothing while closed
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
    player_two::ride_watch::Tick();
    player_two::wheelie_watch::Tick();
    player_two::framing_watch::Tick(splitShown);
    player_two::charapop_watch::Tick(splitShown);
    player_two::handover_watch::Tick();
}

}  // namespace lss::dev
