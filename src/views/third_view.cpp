#include "views/third_view.h"

#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/own_controller.h"
#include "player_two/view_target.h"
#include "players/held_setting.h"
#include "players/unreal_access.h"

#include <optional>

namespace lss::views::third_view {

namespace {

using players::unreal::UObject;

constexpr std::int32_t  kControllerId = 2;  // the third pad's id; no pad is expected on it
constexpr std::uint8_t  kThreeColumns = 2;  // EThreePlayerSplitScreenType::Vertical
constexpr std::uint32_t kStatusTicks  = 300;
constexpr const wchar_t* kLayoutName  = L"ThreePlayerSplitscreenLayout";

// The three-player layout is one fixed field: its held setting's name is for the log only.
std::optional<std::uint8_t> ReadLayout(const wchar_t*) noexcept
{
    return players::unreal::ReadThreePlayerSplitscreenLayout();
}

bool WriteLayout(const wchar_t*, std::uint8_t value) noexcept
{
    return players::unreal::WriteThreePlayerSplitscreenLayout(value);
}

std::uintptr_t g_removeLocalPlayer = 0;
UObject*       g_player            = nullptr;  // the third ULocalPlayer, only ever one this module created
UObject*       g_gameInstance      = nullptr;
void*          g_controller        = nullptr;
players::HeldSetting<std::uint8_t> g_layout{{&ReadLayout, &WriteLayout}, kLayoutName};  // columns while in
std::uint32_t  g_ticks  = 0;
std::uint32_t  g_reaims = 0;

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

bool Listed(RC::Unreal::TArray<UObject*>* players, UObject* player) noexcept
{
    for (std::int32_t i = 0; players != nullptr && i < players->Num(); ++i)
    {
        if ((*players)[i] == player)
        {
            return true;
        }
    }
    return false;
}

void Forget() noexcept
{
    g_player       = nullptr;
    g_gameInstance = nullptr;
    g_controller   = nullptr;
    g_ticks        = 0;
    g_reaims       = 0;
}

}  // namespace

void Install(std::uintptr_t removeLocalPlayer) noexcept
{
    g_removeLocalPlayer = removeLocalPlayer;
    platform::Log(L"third view: ready (Ctrl+F3 or 'third-view on' while player 2 is in; the multi-monitor measurement)");
}

bool In() noexcept
{
    return g_player != nullptr;
}

bool Add() noexcept
{
    if (g_removeLocalPlayer == 0)
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
    UObject* gameInstance = players::unreal::GameInstance();
    auto*    players      = players::unreal::LocalPlayers(gameInstance);
    if (players == nullptr || players->Num() != 2)
    {
        platform::Log(L"third view: expected two local players, found {}", players == nullptr ? -1 : players->Num());
        return false;
    }

    players::unreal::CreatedPlayer created;
    player_two::own_controller::Report report;
    {
        const player_two::own_controller::JoinScope scope(playerOne);
        created = players::unreal::CreatePlayerWithController(static_cast<UObject*>(playerOne), kControllerId);
        report  = scope.Result();
    }
    if (players->Num() != 3)
    {
        platform::Log(L"third view: the engine did not add a third local player ({}; count now {})",
                      created.called ? L"the call returned" : L"the call raised an exception", players->Num());
        return false;
    }
    g_player       = (*players)[2];
    g_gameInstance = gameInstance;
    UObject** field = players::unreal::ControllerField(g_player);
    g_controller    = field == nullptr ? nullptr : *field;
    if (!created.called)
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
                  kControllerId, reinterpret_cast<std::uintptr_t>(g_controller), YesNo(report.cameraManager != nullptr),
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
    auto*      players = players::unreal::LocalPlayers(g_gameInstance);
    const bool listed  = Listed(players, g_player);
    bool       out     = !listed;
    if (listed)
    {
        // RemoveLocalPlayer reports success even for a player it did not hold: membership is the evidence.
        out = players::unreal::RemoveLocalPlayer(g_gameInstance, g_player, g_removeLocalPlayer) && !Listed(players, g_player);
    }
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
    if (!Listed(players::unreal::LocalPlayers(g_gameInstance), g_player))
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
