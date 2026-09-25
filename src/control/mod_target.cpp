#include "control/mod_target.h"

#include "hud/overlay_layout.h"
#include "hud/position_probe.h"
#include "hud/widget_dump.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/pad_sticks.h"
#include "player_two/session.h"
#include "players/split_layout.h"
#include "players/unreal_access.h"
#include "views/player_view_point.h"

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
    if (!session_.HasPlayerTwo())
    {
        return Failed(L"player 2 is not in");
    }
    session_.Leave();  // completes over the following ticks; `wait-until out` confirms it
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
    return {true, on ? L"HUD laid out in each half's visible band" : L"HUD full-image layout (values logged only)"};
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

Outcome ModTarget::Dump(const std::wstring& topic)
{
    // "widgets" or "slots <class>" (control/script.cpp).
    const bool slots  = topic.starts_with(L"slots ");
    const int  listed = slots ? hud::DumpSlots(topic.substr(6)) : topic == L"widgets" ? hud::DumpWidgets() : -1;
    return listed >= 0 ? Outcome{true, std::to_wstring(listed) + (slots ? L" slots" : L" widgets") + L" listed in the log"}
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
