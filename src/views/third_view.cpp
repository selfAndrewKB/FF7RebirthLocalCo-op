#include "views/third_view.h"

#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/own_controller.h"
#include "player_two/view_target.h"
#include "players/held_setting.h"
#include "players/local_players.h"
#include "players/unreal_access.h"

#include <optional>

namespace lss::views::third_view {

namespace {

using players::unreal::UObject;

constexpr std::int32_t   kSeat         = 2;  // the third local player: controller id 2, no pad expected on it
constexpr std::uint8_t   kThreeColumns = 2;  // EThreePlayerSplitScreenType::Vertical
constexpr std::uint32_t  kStatusTicks  = 300;
constexpr const wchar_t* kLayoutName   = L"ThreePlayerSplitscreenLayout";

// The three-player layout is one fixed field: its held setting's name is for the log only.
std::optional<std::uint8_t> ReadLayout(const wchar_t*) noexcept
{
    return players::unreal::ReadThreePlayerSplitscreenLayout();
}

bool WriteLayout(const wchar_t*, std::uint8_t value) noexcept
{
    return players::unreal::WriteThreePlayerSplitscreenLayout(value);
}

std::optional<players::LocalPlayers> g_players;  // the third seat, once installed (players/local_players.h)
void*                                g_controller = nullptr;
players::HeldSetting<std::uint8_t>   g_layout{{&ReadLayout, &WriteLayout}, kLayoutName};  // columns while in
std::uint32_t                        g_ticks  = 0;
std::uint32_t                        g_reaims = 0;

const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

/// Logs a layout change; true if the mod's value is in place after it.
bool LogLayout(const players::HeldChange& change) noexcept
{
    if (const auto text = players::Describe(change, L"third view", kLayoutName); !text.empty())
    {
        platform::Log(L"{}", text);
    }
    return change.kind == players::HeldChange::Kind::Applied || change.kind == players::HeldChange::Kind::AlreadySet;
}

void Forget() noexcept
{
    g_controller = nullptr;
    g_ticks      = 0;
    g_reaims     = 0;
}

}  // namespace

void Install(std::uintptr_t removeLocalPlayer) noexcept
{
    g_players.emplace(removeLocalPlayer, kSeat);
    platform::Log(L"third view: ready (Ctrl+F3 or 'third-view on' while player 2 is in; the multi-monitor measurement)");
}

bool In() noexcept
{
    return g_players && g_players->HasPlayer();
}

bool Add() noexcept
{
    if (!g_players)
    {
        platform::Log(L"third view: not installed");
        return false;
    }
    if (In())
    {
        platform::Log(L"third view: already in");
        return false;
    }
    void* playerOne = player_two::generation::PlayerOneController();
    if (!player_two::generation::Active() || playerOne == nullptr)
    {
        platform::Log(L"third view: player 2 must be in first (the third view borrows player 2's join path)");
        return false;
    }
    if (!player_two::own_controller::Ready())
    {
        platform::Log(L"third view: player 2's camera-manager stage is not ready");
        return false;
    }
    if (g_players->FirstPlayer(true).controller == nullptr)
    {
        return false;  // the seat's precondition (exactly two local players, player 1 with a controller) logged
    }

    players::LocalPlayers::Added     added;
    player_two::own_controller::Report report;
    {
        const player_two::own_controller::JoinScope scope(playerOne);
        added  = g_players->Add(static_cast<UObject*>(playerOne));
        report = scope.Result();
    }
    if (!g_players->HasPlayer())
    {
        return false;  // the engine did not add it: logged by the seat
    }
    g_controller = added.controller;
    if (added.raised)
    {
        platform::Log(L"third view: the engine raised an exception while creating the third player (camera manager "
                      L"handled: {}, owner filled: {}, names swapped: {}); removing it",
                      YesNo(report.cameraManager != nullptr), YesNo(report.ownerFilled), YesNo(report.namesSwapped));
        Remove(L"the create raised");
        return false;
    }

    const bool columns = LogLayout(g_layout.Hold(kThreeColumns));

    void*      target = player_two::view_target::Current(playerOne);
    const bool aimed  = g_controller != nullptr && target != nullptr && player_two::view_target::AimAt(g_controller, target);
    platform::Log(L"third view: a third local player is in (controller id {}, controller {:#x}; camera manager handled {}, "
                  L"owner filled {}, names swapped {}); {}; its camera manager views {:#x} ({}player 1's view "
                  L"target {:#x})",
                  kSeat, reinterpret_cast<std::uintptr_t>(g_controller), YesNo(report.cameraManager != nullptr),
                  YesNo(report.ownerFilled), YesNo(report.namesSwapped),
                  columns ? L"three columns" : L"NOT three columns (the setting could not be read or written)",
                  reinterpret_cast<std::uintptr_t>(player_two::view_target::Current(g_controller)),
                  aimed ? L"" : L"NOT ", reinterpret_cast<std::uintptr_t>(target));
    return true;
}

void Remove(const wchar_t* why) noexcept
{
    if (!In())
    {
        return;
    }
    LogLayout(g_layout.Release());
    // RemoveLocalPlayer reports success even for a player it did not hold: membership before and after is the
    // evidence (the seat's); here only for the line.
    const bool listed = g_players->StillListed();
    const bool out    = g_players->Remove();
    platform::Log(L"third view: {}: the third local player {} (re-aimed at player 1's view target {} time(s) while in)",
                  why, out ? (listed ? L"left" : L"was already gone") : L"could NOT be removed; kept, tried again later",
                  g_reaims);
    if (out)
    {
        Forget();
    }
}

void Tick(bool playerTwoIn) noexcept
{
    if (!In())
    {
        return;
    }
    if (!g_players->StillListed())
    {
        platform::Log(L"third view: the engine removed the third local player by itself");
        Forget();
        LogLayout(g_layout.Release());  // the game's layout back, as at a removal of the mod's own
        return;
    }
    if (!playerTwoIn)
    {
        Remove(L"player 2 is out");
        return;
    }
    void* playerOne = player_two::generation::PlayerOneController();
    void* target    = playerOne == nullptr ? nullptr : player_two::view_target::Current(playerOne);
    void* viewed    = g_controller == nullptr ? nullptr : player_two::view_target::Current(g_controller);
    if (target != nullptr && viewed != target && player_two::view_target::AimAt(g_controller, target))
    {
        ++g_reaims;
        platform::LogDebug(L"third view: re-aimed at player 1's view target {:#x} (viewed {:#x})",
                           reinterpret_cast<std::uintptr_t>(target), reinterpret_cast<std::uintptr_t>(viewed));
    }
    if (++g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        const auto size   = playerOne == nullptr ? std::nullopt : players::unreal::ViewportSizeOf(static_cast<UObject*>(playerOne));
        const auto layout = players::unreal::ReadThreePlayerSplitscreenLayout();
        platform::LogDebug(L"third view: viewport {}x{}, ThreePlayerSplitscreenLayout {}, the third camera manager views "
                           L"{:#x}, player 1's {:#x}; re-aims {}",
                           size ? size->width : -1, size ? size->height : -1, layout ? static_cast<int>(*layout) : -1,
                           reinterpret_cast<std::uintptr_t>(viewed), reinterpret_cast<std::uintptr_t>(target), g_reaims);
    }
}

}  // namespace lss::views::third_view
