#include "views/player_view_point.h"

#include "views/axis_records.h"
#include "platform/engine_addresses.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settled_log.h"
#include "platform/vtable_entry.h"
#include "views/minimal_view_info.h"
#include "views/stream_view_math.h"
#include "views/view_shape.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

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
std::atomic<bool>          g_splitShown{false};  // player 2 in and the screen split (not player 1's alone)
std::atomic<std::uint32_t> g_axisOffset{0};  // 0 until reflection has confirmed it
std::atomic<float>         g_fovWeight{kDefaultSplitFovWeight};  // 0..1; negative: the MajorAxisFOV rule
std::atomic<float>         g_halfAspect{0.0f};       // the split halves' width/height; 0 until known
std::atomic<float>         g_halfHeightRatio{0.0f};  // their height as a fraction of the screen's

/// Player 2's view point, the first time it is computed from a given camera.
void DescribeViewPoint(const std::byte* viewInfo, bool ownCamera) noexcept
{
    float yaw = 0.0f;
    std::memcpy(&yaw, viewInfo + view_info::kYaw, sizeof(yaw));
    const auto previousSet = static_cast<unsigned>(*(viewInfo + view_info::kPreviousViewTransformSet));
    platform::LogDebug(L"player 2's view point from {}: yaw {:.1f}; PreviousViewTransform set = {}",
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
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
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
    platform::LogDebug(L"{}'s view shape (engine): constrain aspect ratio = {} (flag bits 0x{:X}), aspect ratio {:.4f}, "
                       L"FOV {:.1f}",
                       isPlayerTwo ? L"player 2" : L"player 1", (shape.flags & view_info::kConstrainAspectRatioBit) != 0,
                       static_cast<unsigned>(shape.flags), shape.aspectRatio, fov);
}

// ---------------------------------------------------------------------------------------------
// Split players fill their half
// ---------------------------------------------------------------------------------------------
//
// While player 2 is in, every player's view takes the mod's FOV rule
// (ULocalPlayer::AspectRatioAxisConstraint), read by GetProjectionData after this function returns
// (0x140CC0B1D) for a view without the 16:9 constraint. The constraint itself is the main camera's
// own setting, cleared once per camera through the game's setter (player_two/main_camera_shape.h).
//
// The FOV rule is written once per player, at the player's first split frame (and again only if
// the mod's own FOV mode changes), then left alone: the game's only writer, the level-sequence
// camera cut, saves the value before it writes its own and puts that value back when the cut ends
// (RE-p2-hud-view-levers.md §4), so the mod's value comes back by the game's own restore. A value
// the game holds meanwhile is logged, not overwritten.
//
// The FOV rule is only read for a view without the constraint, so with the fill switched off
// (bars) the value left in place makes no difference until it is restored at the leave.

/// The players whose FOV rule the mod changed, the value to put back, the rule the mod wrote, and the
/// value last seen in the field (so each change the game makes is logged once): views/axis_records.h.
constexpr std::size_t     kAxisRecords = 4;  // one per local player the engine allows (4 in stock UE)
AxisRecords<kAxisRecords> g_axisRecords;     // game thread only
bool                  g_refusalLogged = false;
bool                  g_fillLogged    = false;

/// The rule the mod last wrote for any player, so the restore recognises its own value.
bool IsModRule(std::uint8_t value) noexcept
{
    return value == kMajorAxisFov || value == kMaintainXFov;
}

/// Gives `localPlayer` the FOV rule `rule` once: at its first split frame, or when the mod's rule
/// changes. Returns true if the mod's rule was written for this player (false when the offset is
/// unconfirmed or there is no room to remember the original, in which case nothing was changed).
bool ApplyAxisRule(void* localPlayer, std::uint8_t rule) noexcept
{
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    if (offset == 0)
    {
        return false;  // not confirmed by reflection: the engine's rule stays
    }
    auto* field = static_cast<std::uint8_t*>(localPlayer) + offset;
    switch (g_axisRecords.Apply(localPlayer, *field, rule))
    {
        case AxisApplied::NoRoom: return false;  // no room to remember the original, so it is not changed
        case AxisApplied::GamePutBack:
            platform::Log(L"a player's FOV rule is the mod's {} again (the game put it back)", static_cast<unsigned>(*field));
            break;
        case AxisApplied::GameWrote:  // a value the game wrote over the mod's rule (a camera cut's), logged once per change
            platform::Log(L"a player's FOV rule is {} (the game's, e.g. a camera cut); left to the game",
                          static_cast<unsigned>(*field));
            break;
        case AxisApplied::First:
        case AxisApplied::RuleChanged:
        case AxisApplied::Unchanged: break;
    }
    return true;
}

/// The split FOV's log, per player (index 1: player 2): at once for a new weight, half shape or camera
/// aspect ratio; for a new camera FOV once it has held still (to 0.1 degree) for about half a second, so
/// a zoom or a framing blend logs where it ends instead of every frame.
struct FovSteady
{
    float weight = -2.0f;
    float aspect = 0.0f;
    float height = 0.0f;
    float cameraRatio = 0.0f;
    bool operator==(const FovSteady&) const = default;
};
constexpr std::uint32_t kFovStillFrames = 30;
platform::SettledLog<FovSteady, int> g_fovLog[2] = {platform::SettledLog<FovSteady, int>(kFovStillFrames),
                                                    platform::SettledLog<FovSteady, int>(kFovStillFrames)};

/// Each player's half width against the camera's own (tan of the half-angles; 1 without a split FOV), for the
/// camera code that places things across the view (player_two/shoulder_scale.h).
std::atomic<float> g_widthRatio[2] = {1.0f, 1.0f};

/// With a weight chosen and the halves' shape known, writes the split FOV (views/view_shape.h,
/// SplitFov) under the MaintainXFOV rule; otherwise leaves the FOV to the MajorAxisFOV rule.
void ApplySplitFov(void* localPlayer, std::byte* viewInfo, bool isPlayerTwo) noexcept
{
    const float     weight = g_fovWeight.load(std::memory_order_relaxed);
    const HalfShape half{g_halfAspect.load(std::memory_order_relaxed), g_halfHeightRatio.load(std::memory_order_relaxed)};
    if (weight < 0.0f || half.aspect <= 0.0f || half.heightRatio <= 0.0f)
    {
        ApplyAxisRule(localPlayer, kMajorAxisFov);
        g_widthRatio[isPlayerTwo ? 1 : 0].store(1.0f, std::memory_order_relaxed);
        return;
    }
    if (!ApplyAxisRule(localPlayer, kMaintainXFov))
    {
        return;  // the rule could not be set, so a written FOV would be read under the wrong rule
    }
    float fov         = 0.0f;
    float cameraRatio = 0.0f;
    std::memcpy(&fov, viewInfo + view_info::kFov, sizeof(fov));
    std::memcpy(&cameraRatio, viewInfo + view_info::kAspectRatio, sizeof(cameraRatio));
    const float split = SplitFov(fov, cameraRatio, half, weight);
    std::memcpy(viewInfo + view_info::kFov, &split, sizeof(split));
    g_widthRatio[isPlayerTwo ? 1 : 0].store(WidthRatio(split, fov), std::memory_order_relaxed);

    const FovSteady steady{weight, half.aspect, half.heightRatio, cameraRatio};
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_fovLog[isPlayerTwo ? 1 : 0].Reset();  // off: the next line starts fresh
    }
    else if (g_fovLog[isPlayerTwo ? 1 : 0].Offer(steady, static_cast<int>(std::lround(fov * 10.0f))))
    {
        const float vertical = VerticalFovDegrees(split, half.aspect);
        platform::LogDebug(L"split FOV ({}): weight {:.2f}, half aspect {:.3f} (height {:.2f} of the screen): camera "
                           L"{:.1f} deg wide at {:.3f} -> {:.1f} deg wide, {:.1f} deg tall",
                           isPlayerTwo ? L"player 2" : L"player 1", weight, half.aspect, half.heightRatio, fov,
                           cameraRatio, split, vertical);
    }
}

void ShapeSplitView(void* localPlayer, std::byte* viewInfo, bool isPlayerTwo) noexcept
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
    ApplySplitFov(localPlayer, viewInfo, isPlayerTwo);
    if (!g_fillLogged)
    {
        g_fillLogged = true;
        platform::Log(L"split views: the FOV rule is the mod's{} (the halves fill by each main camera's own constraint "
                      L"setting, player_two/main_camera_shape)",
                      g_axisOffset.load(std::memory_order_relaxed) == 0 ? L" NOT: offset unconfirmed" : L"");
    }
}

/// Puts back the FOV rule of every player the mod changed, except `removedPlayer`, which the
/// engine has already let go of and must not be touched, then forgets every record: this runs
/// when player 2 changes, which ends the split for all players at once, and the next split
/// frame records the originals afresh.
void RestoreAxisRules(const void* removedPlayer) noexcept
{
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    g_axisRecords.Restore(
        removedPlayer,
        [offset](const void* player) -> std::uint8_t* {
            return offset != 0 ? static_cast<std::uint8_t*>(const_cast<void*>(player)) + offset : nullptr;
        },
        &IsModRule,
        [](std::uint8_t original) { platform::Log(L"a player's FOV rule restored to {}", static_cast<unsigned>(original)); });
    g_fillLogged = false;
}

/// The engine's view point, then the camera choice and the split's shape.
void GetViewPointShaped(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass)
{
    const void* playerTwo   = g_playerTwo.load(std::memory_order_relaxed);
    const void* playerOne   = g_playerOne.load(std::memory_order_relaxed);
    const bool  isPlayerTwo = playerTwo != nullptr && localPlayer == playerTwo;
    const bool  ownCamera   = g_ownCamera.load(std::memory_order_relaxed);

    // Until player 2's own camera is chosen, player 2's half shows player 1's camera.
    void* source = isPlayerTwo && !ownCamera && playerOne != nullptr ? const_cast<void*>(playerOne) : localPlayer;
    g_engineGetViewPoint(source, viewInfo, stereoPass);

    LogShapeOnChange(isPlayerTwo, viewInfo);
    if (playerTwo == nullptr || !g_splitShown.load(std::memory_order_relaxed))
    {
        return;  // one player, or player 1's single view: exactly the engine's result
    }
    if (g_fillOn.load(std::memory_order_relaxed))
    {
        ShapeSplitView(localPlayer, viewInfo, isPlayerTwo);
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

// Each split player's camera as it was last drawn (index 1: player 2), for the streaming views
// (views/stream_views.h). Game thread (Draw).
std::optional<StreamCamera> g_rendered[2];

/// Same signature as ULocalPlayer::GetViewPoint.
void GetViewPoint(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass)
{
    GetViewPointShaped(localPlayer, viewInfo, stereoPass);
    const void* playerTwo = g_playerTwo.load(std::memory_order_relaxed);
    if (playerTwo != nullptr && (localPlayer == playerTwo || localPlayer == g_playerOne.load(std::memory_order_relaxed)))
    {
        g_rendered[localPlayer == playerTwo ? 1 : 0] = CameraOf(viewInfo);
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
    g_ownCamera.store(playerTwo != nullptr, std::memory_order_relaxed);  // each new player 2 starts on their own camera
    g_describePending.store(playerTwo != nullptr, std::memory_order_relaxed);
    g_shapeLogged[1] = false;  // a new player 2's shape is logged afresh
    g_rendered[0].reset();
    g_rendered[1].reset();
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

bool ShowsOwnCamera() noexcept
{
    return g_ownCamera.load(std::memory_order_relaxed);
}

bool ToggleFill() noexcept
{
    const bool on = !g_fillOn.load(std::memory_order_relaxed);
    g_fillOn.store(on, std::memory_order_relaxed);
    g_fillLogged = false;
    return on;
}

float HalfAspect() noexcept
{
    return g_halfAspect.load(std::memory_order_relaxed);
}

std::optional<StreamCamera> RenderedCamera(bool playerTwo) noexcept
{
    return g_rendered[playerTwo ? 1 : 0];
}

void SetSplitShown(bool shown) noexcept
{
    g_splitShown.store(shown, std::memory_order_relaxed);
}

bool SplitShown() noexcept
{
    return g_splitShown.load(std::memory_order_relaxed);
}

bool FillsHalves() noexcept
{
    return g_fillOn.load(std::memory_order_relaxed);
}

void SetSplitFovWeight(float weight) noexcept
{
    g_fovWeight.store(weight >= 0.0f && weight <= 1.0f ? weight : -1.0f, std::memory_order_relaxed);
}

float SplitFovWeight() noexcept
{
    return g_fovWeight.load(std::memory_order_relaxed);
}

float SplitWidthRatio(bool isPlayerTwo) noexcept
{
    return g_widthRatio[isPlayerTwo ? 1 : 0].load(std::memory_order_relaxed);
}

void SetHalfShape(const HalfShape& half) noexcept
{
    const bool known = half.aspect > 0.0f && half.heightRatio > 0.0f;
    g_halfAspect.store(known ? half.aspect : 0.0f, std::memory_order_relaxed);
    g_halfHeightRatio.store(known ? half.heightRatio : 0.0f, std::memory_order_relaxed);
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
