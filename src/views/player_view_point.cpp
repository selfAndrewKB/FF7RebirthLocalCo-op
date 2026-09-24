#include "views/player_view_point.h"

#include "platform/engine_addresses.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "views/view_turn.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::views {

namespace {

/// ULocalPlayer::GetViewPoint(this, FMinimalViewInfo& OutViewInfo, EStereoscopicPass StereoPass).
using GetViewPointFn = void (*)(const void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass);

// Set once before the entry is replaced, then only read.
platform::VTableEntry g_entry;
GetViewPointFn        g_engineGetViewPoint = nullptr;
std::uintptr_t        g_localPlayerVTable  = 0;

// Written on the game thread by SetPlayerTwo/ToggleTestTurn, read by the replacement. The engine
// calls GetViewPoint from Draw, also on the game thread; atomics keep that true if it ever changes.
std::atomic<const void*> g_playerTwo{nullptr};
std::atomic<bool>        g_turnOn{false};
std::atomic<bool>        g_describePending{false};

/// The first view point computed for a new player 2, as the engine made it (before any turn).
void DescribeFirstViewPoint(const std::byte* viewInfo) noexcept
{
    float yaw = 0.0f;
    std::memcpy(&yaw, viewInfo + view_info::kYaw, sizeof(yaw));
    const auto previousSet = static_cast<unsigned>(*(viewInfo + view_info::kPreviousViewTransformSet));
    platform::Log(L"player 2's view point, as the engine computed it: yaw {:.1f}; PreviousViewTransform set = {} "
                  L"(copied from player 1's camera; nonzero would reproject player 2's history from player 1's)",
                  yaw, previousSet);
}

/// Same signature as ULocalPlayer::GetViewPoint.
void GetViewPoint(const void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass)
{
    g_engineGetViewPoint(localPlayer, viewInfo, stereoPass);

    const void* playerTwo = g_playerTwo.load(std::memory_order_relaxed);
    if (localPlayer != playerTwo || playerTwo == nullptr)
    {
        return;  // everyone but player 2: exactly the engine's result
    }
    if (g_describePending.exchange(false, std::memory_order_relaxed))
    {
        DescribeFirstViewPoint(viewInfo);
    }
    if (g_turnOn.load(std::memory_order_relaxed))
    {
        TurnIfPlayer(localPlayer, playerTwo, kTestTurnDegrees, viewInfo);
    }
}

}  // namespace

bool InstallPlayerViewPoint(const platform::GameImage& image) noexcept
{
    const auto entry    = image.Rebase(engine::kGetViewPointEntry, sizeof(void*));
    const auto engineFn = image.Rebase(engine::kGetViewPoint);
    const auto vtable   = image.Rebase(engine::kLocalPlayerVTable);
    if (!entry || !engineFn || !vtable)
    {
        return false;
    }
    g_engineGetViewPoint = reinterpret_cast<GetViewPointFn>(*engineFn);
    g_localPlayerVTable  = *vtable;
    return g_entry.Replace(*entry, *engineFn, reinterpret_cast<std::uintptr_t>(&GetViewPoint));
}

void RemovePlayerViewPoint() noexcept
{
    if (!g_entry.Restore())
    {
        platform::Log(L"could not restore ULocalPlayer's GetViewPoint entry (another mod changed it after us); "
                      L"the replacement stays in place");
    }
}

void SetPlayerTwo(const void* localPlayer) noexcept
{
    if (g_playerTwo.exchange(localPlayer, std::memory_order_relaxed) == localPlayer)
    {
        return;
    }
    g_turnOn.store(false, std::memory_order_relaxed);
    g_describePending.store(localPlayer != nullptr, std::memory_order_relaxed);
}

bool ToggleTestTurn() noexcept
{
    if (g_playerTwo.load(std::memory_order_relaxed) == nullptr)
    {
        g_turnOn.store(false, std::memory_order_relaxed);
        return false;
    }
    const bool on = !g_turnOn.load(std::memory_order_relaxed);
    g_turnOn.store(on, std::memory_order_relaxed);
    return on;
}

bool UsesEngineViewPoint(const void* localPlayer) noexcept
{
    if (localPlayer == nullptr)
    {
        return false;
    }
    std::uintptr_t vtable = 0;  // an object's first 8 bytes are its vtable pointer
    std::memcpy(&vtable, localPlayer, sizeof(vtable));
    return vtable == g_localPlayerVTable;
}

}  // namespace lss::views
