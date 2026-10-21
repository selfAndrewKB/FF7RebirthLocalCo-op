#include "dev/dev_tools.h"

#include "control/command_channel.h"
#include "control/mod_target.h"
#include "hud/hud_trace.h"
#include "hud/label_relayouts.h"
#include "hud/position_probe.h"
#include "platform/log.h"
#include "player_two/charapop_watch.h"
#include "player_two/framing_watch.h"
#include "player_two/handover_watch.h"
#include "player_two/menu_focus.h"
#include "player_two/ride_watch.h"
#include "player_two/rumble_watch.h"
#include "player_two/tag_counts.h"
#include "player_two/wheelie_watch.h"
#include "renderer/submission_census.h"

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
    // The watches: each reads and logs, changes nothing, and reports its own install (its header says what it reads).
    player_two::rumble_watch::Install(image);    // each controller's rumble, pad by pad
    player_two::ride_watch::Install(image);      // the chocobo rides and the vehicle coordinators
    player_two::wheelie_watch::Install(image);   // the Wheelie system
    player_two::framing_watch::Install(image);   // each player's field-camera framing
    player_two::charapop_watch::Install(image);  // the character detail near player 2
    player_two::handover_watch::Install(image);  // the party order and the members' AI at hand-overs
    player_two::menu_focus::Install(image);      // player 1's UI focus, on change
    const bool tags = player_two::tag_counts::Install(image);  // the actor-tag registry (the session's Debug counts)
    platform::Log(L"tag counts: {}", tags ? L"readable (the session's take and leave counts, Debug)"
                                          : L"NOT readable (a check failed); the session's counts are not logged");
    hud::label_relayouts::Install(image);        // which text blocks relayout, where and why (the minimap marks the scopes)
    renderer::InstallSubmissionCensusIfAsked();  // the GPU submission census: census.txt at launch, or `census on`
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
    hud::label_relayouts::Remove();
    player_two::menu_focus::Remove();
    renderer::RemoveSubmissionCensus();
}

void TickBeforeSession() noexcept
{
    if (g_target)
    {
        g_channel.Tick(*g_target);
    }
}

void TickAfterSession() noexcept
{
    if (g_target)
    {
        g_target->DropEndedHolds();
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

void BeforeOverlayRender() noexcept
{
    hud::TraceBeforeRender();
}

void AfterOverlayRender() noexcept
{
    hud::TraceAfterRender();
}

}  // namespace lss::dev
