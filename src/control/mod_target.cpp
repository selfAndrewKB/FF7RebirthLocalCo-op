#include "control/mod_target.h"

#include "hud/area_icons_two.h"
#include "hud/hud_players_dump.h"
#include "hud/hud_trace.h"
#include "hud/minimap_once.h"
#include "hud/overlay_layout.h"
#include "hud/root_overlay_render.h"
#include "hud/player_two_pass.h"
#include "hud/second_image.h"
#include "hud/position_probe.h"
#include "hud/widget_dump.h"
#include "platform/frame_watch.h"
#include "platform/log.h"
#include "player_two/battle_input.h"
#include "player_two/camera_distance.h"
#include "player_two/charapop_centre.h"
#include "player_two/charapop_near.h"
#include "player_two/shoulder_scale.h"
#include "player_two/generation.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/scope_leaks.h"
#include "player_two/session.h"
#include "players/single_view.h"
#include "players/split_layout.h"
#include "players/unreal_access.h"
#include "renderer/me_feedback_once.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"

namespace lss::control {

namespace {

Outcome Failed(std::wstring why)
{
    return {false, std::move(why)};
}

/// The controller id of player 1's or player 2's controller, or -1.
std::int32_t ControllerIdOfPlayer(std::int32_t player) noexcept
{
    void* controller =
        player == 1 ? player_two::generation::PlayerOneController() : player_two::generation::PlayerTwoController();
    return controller == nullptr ? -1 : player_two::pad_sticks::ControllerIdOf(controller);
}

}  // namespace

bool ModTarget::HasPlayerTwo() const
{
    return session_.HasPlayerTwo();
}

bool ModTarget::PlayerTwoPlaying() const
{
    return session_.PlaysMember();
}

Outcome ModTarget::Join()
{
    if (session_.HasPlayerTwo())
    {
        return Failed(L"player 2 is already in");
    }
    session_.Join();  // logs its own outcome; `wait-until playing` confirms it
    return {};
}

Outcome ModTarget::Leave()
{
    if (!session_.HasPlayerTwo() && !session_.AwaitingMember())
    {
        return Failed(L"player 2 is not in");
    }
    session_.Leave();  // completes over the following ticks (or stops a wait); `wait-until out` confirms it
    return {};
}

Outcome ModTarget::SetOwnCamera(bool on)
{
    if (!viewPointInstalled_)
    {
        return Failed(L"the view-point entry is not installed");
    }
    if (!session_.HasPlayerTwo())
    {
        return Failed(L"player 2 is not in");
    }
    if (views::ShowsOwnCamera() != on)
    {
        views::ToggleOwnCamera();
    }
    return {true, views::ShowsOwnCamera() ? L"player 2's own camera" : L"player 1's camera"};
}

Outcome ModTarget::SetPerPadSticks(bool on)
{
    if (!player_two::pad_sticks::Installed())
    {
        return Failed(L"the per-pad sticks stage is not installed");
    }
    if (player_two::pad_sticks::Enabled() != on)
    {
        session_.TogglePadSticks();  // logs the new state
    }
    return player_two::pad_sticks::Enabled() == on ? Outcome{} : Failed(L"the switch did not take");
}

Outcome ModTarget::SetLayout(bool sideBySide)
{
    using Kind         = players::LayoutChange::Kind;
    const auto value   = static_cast<std::uint8_t>(sideBySide ? players::SplitLayoutKind::SideBySide
                                                              : players::SplitLayoutKind::TopBottom);
    const auto change  = layout_.Choose(value);
    const wchar_t* name = sideBySide ? L"side by side" : L"top/bottom";
    switch (change.kind)
    {
        case Kind::Applied: return {true, name};
        case Kind::Chosen: return {true, std::wstring(name) + L" from the next join"};
        case Kind::ApplyFailed: return Failed(L"TwoPlayerSplitscreenLayout could not be read or set");
        default: return Failed(L"unexpected layout outcome");
    }
}

Outcome ModTarget::SetBars(bool on)
{
    if (!viewPointInstalled_)
    {
        return Failed(L"the view-point entry is not installed");
    }
    if (views::FillsHalves() == on)  // bars on means the halves do not fill
    {
        views::ToggleFill();
    }
    return {true, views::FillsHalves() ? L"halves filled" : L"16:9 halves with bars"};
}

Outcome ModTarget::SetHudBand(bool on)
{
    if (!hud::OverlayLayoutInstalled())
    {
        return Failed(L"the HUD layout is not installed (see the startup log)");
    }
    hud::SetBandEnabled(on);
    // What it sets is logged by the HUD module at the overlay's next render while split.
    return {true, on ? L"HUD laid out in each half's visible band (the default)"
                     : L"HUD full-image layout until `hud-band on` (values logged only)"};
}

Outcome ModTarget::SetNpcCentre(bool on)
{
    if (!player_two::charapop_centre::Installed())
    {
        return Failed(L"the NPC centre is not installed (see the startup log)");
    }
    player_two::charapop_centre::SetEnabled(on);  // logs the new state
    return {true, on ? L"the NPC population follows both players while split (the default)"
                     : L"the NPC population follows the leader, as the game alone does"};
}

Outcome ModTarget::SetNpcDetail(std::uint32_t level)
{
    using player_two::charapop_near::Detail;
    if (!player_two::charapop_near::Installed())
    {
        return Failed(L"the NPC detail lever is not installed (see the startup log)");
    }
    const Detail detail = level >= 2 ? Detail::Full : level == 1 ? Detail::Reduced : Detail::Off;
    player_two::charapop_near::SetDetail(detail);  // logs the new state
    return {true, detail == Detail::Full    ? L"characters near player 2 keep full detail where player 1's camera does not (the default)"
                  : detail == Detail::Reduced ? L"characters near player 2 keep full detail within half the game's distances"
                                              : L"characters near player 2 get only what player 1's camera gives them"};
}

Outcome ModTarget::SetHudProbe(bool on)
{
    if (!hud::PositionProbeInstalled())
    {
        return Failed(L"the HUD position probe is not installed (see the startup log)");
    }
    hud::SetPositionProbe(on);
    return {};
}

Outcome ModTarget::TraceHud(std::int32_t frames)
{
    if (!hud::HudTraceInstalled() || !hud::RootOverlayRenderInstalled())
    {
        return Failed(L"the HUD trace or the overlay render hook is not installed (see the startup log)");
    }
    hud::StartHudTrace(frames);
    return {true, L"tracing (one log line per frame)"};
}

Outcome ModTarget::SetHudImage(bool on)
{
    if (!hud::SecondImageInstalled() || !hud::RootOverlayRenderInstalled())
    {
        return Failed(L"player 2's HUD image is not installed (see the startup log)");
    }
    hud::SetSecondImage(on);
    if (on && !hud::SecondImageOn())
    {
        return Failed(L"player 2's HUD image refused a check (see the log)");
    }
    return {true, on ? L"player 2's HUD image on" : L"player 2's HUD image off"};
}

Outcome ModTarget::SetHudLevers(bool on)
{
    if (!hud::PlayerTwoPassInstalled())
    {
        return Failed(L"player 2's HUD levers are not installed (see the startup log)");
    }
    hud::SetPlayerTwoContent(on);
    if (on && !hud::PlayerTwoContentOn())
    {
        return Failed(L"player 2's HUD levers refused a check (see the log)");
    }
    return {true, on ? (hud::SecondImageOn() ? L"player 2's content on" : L"player 2's content on (needs hud-image on)")
                     : L"player 2's content off"};
}

Outcome ModTarget::SetHudLeversMask(std::uint32_t mask)
{
    hud::SetPlayerTwoLeversMask(mask);
    return {true, L"levers mask set (see the log)"};
}

Outcome ModTarget::SetSticksMask(std::uint32_t mask)
{
    player_two::pad_sticks::SetMask(mask);
    return {true, L"sticks mask set (see the log)"};
}

Outcome ModTarget::SetLeakFixes(std::uint32_t mask)
{
    player_two::scope_leaks::SetMask(mask);
    return {true, L"leak fixes set (see the log)"};
}

Outcome ModTarget::SetButtonsMask(std::uint32_t mask)
{
    player_two::pad_buttons::SetMask(mask);
    return {true, L"buttons mask set (see the log)"};
}

Outcome ModTarget::SetIconsMask(std::uint32_t mask)
{
    hud::area_icons_two::SetMask(mask);
    return {true, L"icons mask set (see the log)"};
}

Outcome ModTarget::SetBattleMask(std::uint32_t mask)
{
    player_two::battle_input::SetMask(mask);
    return {true, L"battle mask set (see the log)"};
}

Outcome ModTarget::SetPerfLog(bool on)
{
    platform::frame_watch::SetDetailed(on);
    return {true, on ? L"perf log on" : L"perf log off"};
}

Outcome ModTarget::SetSingleView(std::uint32_t mode)
{
    constexpr players::SingleViewMode kModes[] = {players::SingleViewMode::Auto, players::SingleViewMode::Single,
                                                  players::SingleViewMode::Split};
    constexpr const wchar_t* kNames[] = {L"single view for the game's whole-screen states (auto)",
                                         L"player 1's single view while player 2 is in", L"always split"};
    if (mode >= std::size(kModes))
    {
        return Failed(L"unknown single-view mode");
    }
    singleView_.SetMode(kModes[mode]);  // applied at the next tick, which logs the change
    return {true, kNames[mode]};
}

Outcome ModTarget::SetStreamViews(std::uint32_t mode)
{
    constexpr views::StreamViewsMode kModes[] = {views::StreamViewsMode::Off, views::StreamViewsMode::PlayerTwo,
                                                 views::StreamViewsMode::Both};
    constexpr const wchar_t* kNames[] = {L"no streaming views (the game's own only)", L"player 2's camera",
                                         L"both players' cameras"};
    if (mode >= std::size(kModes))
    {
        return Failed(L"unknown stream-views mode");
    }
    if (!views::StreamViewsInstalled())
    {
        return Failed(L"streaming views are not installed (see the install log)");
    }
    views::SetStreamViewsMode(kModes[mode]);  // applied at the next tick, which logs it
    return {true, kNames[mode]};
}

Outcome ModTarget::SetPlayerOneStreamFov(std::uint32_t degrees)
{
    if (!views::StreamViewsInstalled())
    {
        return Failed(L"streaming views are not installed (see the install log)");
    }
    views::SetPlayerOneStreamFov(static_cast<float>(degrees));  // applied at the next tick, which logs it
    return {true, degrees == 0 ? std::wstring(L"player 1's camera as its half covers")
                               : std::format(L"player 1's camera {} degrees wide", degrees)};
}

Outcome ModTarget::SetStreamFeedbackOnce(bool playerOneOnly)
{
    if (!renderer::MeFeedbackOnceInstalled())
    {
        return Failed(L"the one-copy streaming feedback is not installed (see the install log)");
    }
    renderer::SetMeFeedbackOnce(playerOneOnly);
    return {true, playerOneOnly ? L"player 1's family alone hands in streaming requests"
                                : L"each family hands in streaming requests (the game's code as it runs per family)"};
}

Outcome ModTarget::SetMinimapOnce(bool on)
{
    if (!hud::MinimapOnceInstalled())
    {
        return Failed(L"the minimap's single render is not installed (see the install log)");
    }
    hud::SetMinimapOnce(on);
    return {true, on ? L"the minimap's map rendered once per frame, by player 1's pass"
                     : L"the minimap's map rendered in both players' passes"};
}

Outcome ModTarget::SetFramingOffsets(std::int32_t topBottom, std::int32_t sideBySide)
{
    if (!player_two::camera_distance::FramingAvailable())
    {
        return Failed(L"split framing is not available (see the install log)");
    }
    player_two::camera_distance::SetLayoutOffsets(topBottom, sideBySide);
    return {true, L"each player's field camera follows the Options distance plus the layout's steps while player 2 is in"};
}

Outcome ModTarget::SetShoulderScale(bool on)
{
    if (!player_two::shoulder_scale::Installed())
    {
        return Failed(L"the shoulder scale is not installed (see the install log)");
    }
    player_two::shoulder_scale::SetOn(on);
    return {true, on ? L"each half's camera shoulder offset scaled to its width" : L"the game's shoulder offset as it is"};
}

Outcome ModTarget::SetShoulderScaleMinimum(float minimum)
{
    if (!player_two::shoulder_scale::Installed())
    {
        return Failed(L"the shoulder scale is not installed (see the install log)");
    }
    player_two::shoulder_scale::SetMinimum(minimum);
    return {true, L"the narrowest half's shoulder offset is softened no further than this"};
}

Outcome ModTarget::Dump(const std::wstring& topic)
{
    // "widgets", "hud-players" or "slots <class>" (control/script.cpp).
    const bool slots  = topic.starts_with(L"slots ");
    const int  listed = slots                     ? hud::DumpSlots(topic.substr(6))
                        : topic == L"widgets"     ? hud::DumpWidgets()
                        : topic == L"hud-players" ? hud::DumpHudPlayers()
                                                  : -1;
    const wchar_t* what = slots ? L" slots" : topic == L"widgets" ? L" widgets" : L" lines";
    return listed >= 0 ? Outcome{true, std::to_wstring(listed) + what + L" listed in the log"}
                       : Failed(L"nothing dumped (see the log)");
}

Outcome ModTarget::SetSplitFov(float blend)
{
    if (!viewPointInstalled_)
    {
        return Failed(L"the view-point entry is not installed");
    }
    views::SetSplitFovWeight(blend);
    // The resulting FOV is logged by the views module when a filled split view first uses it.
    return {true, blend < 0.0f ? L"stock MajorAxisFOV rule" : L"weight set; applies to filled halves"};
}

Outcome ModTarget::HoldSticks(std::int32_t player, const float (&axes)[4])
{
    if (!player_two::pad_sticks::Enabled())
    {
        return Failed(L"per-pad sticks are off (run 'sticks per-pad' first)");
    }
    if (!player_two::generation::Active())
    {
        return Failed(L"player 2 is not in, so no stick is scoped per pad");
    }
    const std::int32_t id = ControllerIdOfPlayer(player);
    if (id < 0)
    {
        return Failed(L"that player's controller id cannot be read");
    }
    ReleaseSticks(player);  // a player's earlier hold may be on another id
    const player_two::Sticks sticks{axes[0], axes[1], axes[2], axes[3]};
    player_two::pad_sticks::SetStickOverride(id, &sticks);
    heldIds_[static_cast<std::size_t>(player - 1)]   = id;
    heldJoins_[static_cast<std::size_t>(player - 1)] = session_.JoinCount();
    return {true, L"controller id " + std::to_wstring(id)};
}

void ModTarget::DropEndedHolds() noexcept
{
    const bool active = player_two::generation::Active();
    for (std::size_t i = 0; i < heldIds_.size(); ++i)
    {
        if (heldIds_[i] >= 0 && (!active || heldJoins_[i] != session_.JoinCount()))
        {
            ReleaseSticks(static_cast<std::int32_t>(i + 1));
        }
    }
}

void ModTarget::ReleaseSticks(std::int32_t player)
{
    // The id recorded at the hold, not a fresh read: after a leave the controller is gone.
    auto& id = heldIds_[static_cast<std::size_t>(player - 1)];
    if (id >= 0)
    {
        player_two::pad_sticks::SetStickOverride(id, nullptr);
        id = -1;
    }
}

Outcome ModTarget::Console(const std::wstring& command)
{
    platform::Log(L"control: console command '{}'", command);
    return players::unreal::ExecuteConsoleCommand(command.c_str()) ? Outcome{}
                                                                    : Failed(L"the console command could not be called");
}

Outcome ModTarget::Quit()
{
    if (session_.HasPlayerTwo())
    {
        return Failed(L"player 2 is still in ('leave' and 'wait-until out' first)");
    }
    platform::Log(L"control: quitting the game through the engine's quit command");
    return players::unreal::ExecuteConsoleCommand(L"quit") ? Outcome{}
                                                            : Failed(L"the quit command could not be called");
}

Outcome ModTarget::Status()
{
    if (!session_.HasPlayerTwo())
    {
        platform::Log(L"control: status: player 2 is not in");
        return {true, L"player 2 is not in"};
    }
    session_.LogStatus();
    return {};
}

Outcome ModTarget::Note(const std::wstring& text)
{
    platform::Log(L"control note: {}", text);
    return {};
}

}  // namespace lss::control
