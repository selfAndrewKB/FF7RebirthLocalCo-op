#include "views/player_view_point.h"

#include "platform/engine_addresses.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "views/minimal_view_info.h"
#include "views/view_shape.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::views {

namespace {

/// ULocalPlayer::GetViewPoint(this, FMinimalViewInfo& OutViewInfo, EStereoscopicPass StereoPass).
using GetViewPointFn = void (*)(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass);

// Set once before the entry is replaced, then only read.
platform::VTableEntry g_entry;
GetViewPointFn        g_engineGetViewPoint = nullptr;
std::uintptr_t        g_localPlayerVTable  = 0;

// Written on the game thread by SetPlayers/ToggleOwnCamera/ToggleFill, read by the replacement.
// The engine calls GetViewPoint from Draw, also on the game thread; atomics keep that true if it
// ever changes.
std::atomic<const void*>   g_playerOne{nullptr};
std::atomic<const void*>   g_playerTwo{nullptr};
std::atomic<bool>          g_ownCamera{false};
std::atomic<bool>          g_describePending{false};
std::atomic<bool>          g_fillOn{true};
std::atomic<std::uint32_t> g_axisOffset{0};  // 0 until reflection has confirmed it

/// Player 2's view point, the first time it is computed from a given camera.
void DescribeViewPoint(const std::byte* viewInfo, bool ownCamera) noexcept
{
    float yaw = 0.0f;
    std::memcpy(&yaw, viewInfo + view_info::kYaw, sizeof(yaw));
    const auto previousSet = static_cast<unsigned>(*(viewInfo + view_info::kPreviousViewTransformSet));
    platform::Log(L"player 2's view point from {}: yaw {:.1f}; PreviousViewTransform set = {}",
                  ownCamera ? L"player 2's own camera" : L"player 1's camera", yaw, previousSet);
}

// ---------------------------------------------------------------------------------------------
// The view's shape, as the engine chose it (logged before anything changes it)
// ---------------------------------------------------------------------------------------------

/// The side bars' inputs: the two defined flag bits and the ratio the view is held to.
struct ViewShape
{
    float         aspectRatio = 0.0f;
    unsigned char flags       = 0;

    bool operator==(const ViewShape&) const = default;
};

// Game thread only (GetViewPoint is called from Draw). Index 0: every player but player 2.
ViewShape g_loggedShape[2]{};
bool      g_shapeLogged[2]{};

/// Logs the shape the first time each player is seen and whenever the bars' inputs change. The
/// FOV is logged with it but does not trigger a line, since the game's camera changes it often.
void LogShapeOnChange(bool isPlayerTwo, const std::byte* viewInfo) noexcept
{
    ViewShape shape{};
    std::memcpy(&shape.aspectRatio, viewInfo + view_info::kAspectRatio, sizeof(shape.aspectRatio));
    shape.flags = static_cast<unsigned char>(*(viewInfo + view_info::kFlags)) & view_info::kKnownFlagBits;

    const auto slot = isPlayerTwo ? 1 : 0;
    if (g_shapeLogged[slot] && g_loggedShape[slot] == shape)
    {
        return;
    }
    g_shapeLogged[slot] = true;
    g_loggedShape[slot] = shape;

    float fov = 0.0f;
    std::memcpy(&fov, viewInfo + view_info::kFov, sizeof(fov));
    platform::Log(L"{}'s view shape (engine): constrain aspect ratio = {} (flag bits 0x{:X}), aspect ratio {:.4f}, "
                  L"FOV {:.1f}",
                  isPlayerTwo ? L"player 2" : L"player 1", (shape.flags & view_info::kConstrainAspectRatioBit) != 0,
                  static_cast<unsigned>(shape.flags), shape.aspectRatio, fov);
}

// ---------------------------------------------------------------------------------------------
// Split players fill their half
// ---------------------------------------------------------------------------------------------
//
// While player 2 is in, every player's view drops the 16:9 constraint and takes the FOV rule
// MajorAxisFOV. Both are read by GetProjectionData after this function returns, from the same
// struct and the same player (0x140CC0B1D), so setting them here on every call also outlasts a
// level-sequence camera cut, which rewrites player 1's FOV rule during the world tick.
//
// The FOV rule is only read for a view without the constraint, so with the fill switched off
// (bars) the value left in place makes no difference until it is restored at the leave.

/// A player whose FOV rule the mod changed, and the value to put back.
struct AxisRecord
{
    const void*  player   = nullptr;
    std::uint8_t original = 0;
};
constexpr std::size_t kAxisRecords = 4;  // one per local player the engine allows (4 in stock UE)
AxisRecord            g_axisRecords[kAxisRecords]{};  // game thread only
bool                  g_refusalLogged = false;
bool                  g_fillLogged    = false;

void ApplyAxisRule(void* localPlayer) noexcept
{
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    if (offset == 0)
    {
        return;  // not confirmed by reflection: the engine's rule stays
    }
    auto* field = static_cast<std::uint8_t*>(localPlayer) + offset;
    if (*field == kMajorAxisFov)
    {
        return;
    }
    AxisRecord* free = nullptr;
    for (auto& record : g_axisRecords)
    {
        if (record.player == localPlayer)
        {
            *field = kMajorAxisFov;  // the original is already kept (e.g. a camera cut rewrote it)
            return;
        }
        if (record.player == nullptr && free == nullptr)
        {
            free = &record;
        }
    }
    if (free == nullptr)
    {
        return;  // no room to remember the original, so it is not changed
    }
    *free  = {localPlayer, *field};
    *field = kMajorAxisFov;
}

void ShapeSplitView(void* localPlayer, std::byte* viewInfo) noexcept
{
    if (!LooksLikeViewInfo(viewInfo))
    {
        if (!g_refusalLogged)
        {
            g_refusalLogged = true;
            platform::Log(L"WARNING: a view's FMinimalViewInfo does not have the expected shape; it is left as the "
                          L"engine made it (bars stay). Logged once.");
        }
        return;
    }
    const bool hadBars = FillHalf(viewInfo);
    ApplyAxisRule(localPlayer);
    if (hadBars && !g_fillLogged)
    {
        g_fillLogged = true;
        platform::Log(L"split views fill their halves (constrain flag cleared, FOV rule MajorAxisFOV{})",
                      g_axisOffset.load(std::memory_order_relaxed) == 0 ? L" NOT applied: offset unconfirmed" : L"");
    }
}

/// Puts back the FOV rule of every player the mod changed, except `removedPlayer`, which the
/// engine has already let go of and must not be touched, then forgets every record: this runs
/// when player 2 changes, which ends the split for all players at once, and the next split
/// frame records the originals afresh.
void RestoreAxisRules(const void* removedPlayer) noexcept
{
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    for (auto& record : g_axisRecords)
    {
        if (record.player != nullptr && record.player != removedPlayer && offset != 0)
        {
            auto* field = static_cast<std::uint8_t*>(const_cast<void*>(record.player)) + offset;
            if (*field == kMajorAxisFov)
            {
                *field = record.original;
                platform::Log(L"a player's FOV rule restored to {}", static_cast<unsigned>(record.original));
            }
        }
        record = {};
    }
    g_fillLogged = false;
}

/// Same signature as ULocalPlayer::GetViewPoint.
void GetViewPoint(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass)
{
    const void* playerTwo   = g_playerTwo.load(std::memory_order_relaxed);
    const void* playerOne   = g_playerOne.load(std::memory_order_relaxed);
    const bool  isPlayerTwo = playerTwo != nullptr && localPlayer == playerTwo;
    const bool  ownCamera   = g_ownCamera.load(std::memory_order_relaxed);

    // Until player 2's own camera is chosen, player 2's half shows player 1's camera.
    void* source = isPlayerTwo && !ownCamera && playerOne != nullptr ? const_cast<void*>(playerOne) : localPlayer;
    g_engineGetViewPoint(source, viewInfo, stereoPass);

    LogShapeOnChange(isPlayerTwo, viewInfo);
    if (playerTwo == nullptr)
    {
        return;  // one player: exactly the engine's result
    }
    if (g_fillOn.load(std::memory_order_relaxed))
    {
        ShapeSplitView(localPlayer, viewInfo);
    }
    if (!isPlayerTwo)
    {
        return;
    }
    if (g_describePending.exchange(false, std::memory_order_relaxed))
    {
        DescribeViewPoint(viewInfo, ownCamera);
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

void SetPlayers(const void* playerOne, const void* playerTwo) noexcept
{
    g_playerOne.store(playerTwo == nullptr ? nullptr : playerOne, std::memory_order_relaxed);
    const void* previous = g_playerTwo.exchange(playerTwo, std::memory_order_relaxed);
    if (previous == playerTwo)
    {
        return;
    }
    if (previous != nullptr)
    {
        RestoreAxisRules(previous);  // same thread as the replacement, so no call is in flight
    }
    g_ownCamera.store(false, std::memory_order_relaxed);
    g_describePending.store(playerTwo != nullptr, std::memory_order_relaxed);
    g_shapeLogged[1] = false;  // a new player 2's shape is logged afresh
}

void SetAxisConstraintOffset(std::uint32_t offset) noexcept
{
    g_axisOffset.store(offset, std::memory_order_relaxed);
}

bool ToggleOwnCamera() noexcept
{
    if (g_playerTwo.load(std::memory_order_relaxed) == nullptr)
    {
        g_ownCamera.store(false, std::memory_order_relaxed);
        return false;
    }
    const bool own = !g_ownCamera.load(std::memory_order_relaxed);
    g_ownCamera.store(own, std::memory_order_relaxed);
    g_describePending.store(true, std::memory_order_relaxed);
    return own;
}

bool ToggleFill() noexcept
{
    const bool on = !g_fillOn.load(std::memory_order_relaxed);
    g_fillOn.store(on, std::memory_order_relaxed);
    g_fillLogged = false;
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
