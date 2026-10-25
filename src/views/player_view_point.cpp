#include "views/player_view_point.h"

#include "facts/engine.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settled_log.h"
#include "platform/vtable_entry.h"
#include "players/split_layout.h"
#include "players/split_settings_access.h"
#include "players/unreal_access.h"
#include "views/axis_records.h"
#include "views/minimal_view_info.h"
#include "views/stream_view_math.h"
#include "views/view_shape.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <Unreal/CoreUObject/UObject/Class.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace lss::views {

namespace {

/// ULocalPlayer::GetViewPoint(this, FMinimalViewInfo& OutViewInfo, EStereoscopicPass StereoPass).
using GetViewPointFn = void (*)(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass);

// Set once before the entry is replaced, then only read.
platform::VTableEntry g_entry;
GetViewPointFn        g_engineGetViewPoint = nullptr;
std::uintptr_t        g_localPlayerVTable  = 0;
bool                  g_installed          = false;  // the entry holds the replacement

// Written on the game thread by Follow/ToggleOwnCamera/ToggleFill/SetSplitFovWeight, read by the replacement.
// The engine calls GetViewPoint from Draw, also on the game thread; atomics keep that true if it
// ever changes.
constexpr std::size_t kSeats = players::kMaxPlayers;  // the per-seat records' size (index 0, player 1's, where used)
std::atomic<const void*>   g_playerOne{nullptr};  // set while a further player is in
std::array<std::atomic<const void*>, kSeats> g_seat{};          // each further player's ULocalPlayer (index 0 unused)
std::array<std::atomic<bool>, kSeats>        g_ownCamera{};     // the further player's view shows their own camera
std::array<std::atomic<bool>, kSeats>        g_describePending{};
std::atomic<bool>          g_onPlayerOne{false};  // every further player's view shows player 1's camera (columns held)
std::atomic<bool>          g_fillOn{true};
std::atomic<bool>          g_splitShown{false};  // a further player in and their own view on screen
std::atomic<bool>          g_viewsSplit{false};  // more than one view on screen (the HUD per column)
std::atomic<std::uint32_t> g_axisOffset{0};  // 0 until reflection has confirmed it
std::atomic<float>         g_fovWeight{kDefaultSplitFovWeight};  // 0..1; negative: the MajorAxisFOV rule
std::atomic<float>         g_halfAspect{0.0f};       // the split halves' width/height; 0 until known
std::atomic<float>         g_halfHeightRatio{0.0f};  // their height as a fraction of the screen's

/// A further player's view point, the first time it is computed from a given camera.
void DescribeViewPoint(int seat, const std::byte* viewInfo, bool ownCamera) noexcept
{
    float yaw = 0.0f;
    std::memcpy(&yaw, viewInfo + view_info::kYaw, sizeof(yaw));
    const auto previousSet = static_cast<unsigned>(*(viewInfo + view_info::kPreviousViewTransformSet));
    platform::LogDebug(L"{}'s view point from {}: yaw {:.1f}; PreviousViewTransform set = {}", players::SeatName(seat),
                       ownCamera ? L"their own camera" : L"player 1's camera", yaw, previousSet);
}

/// `localPlayer`'s seat: 0 player 1's, a further player's, or -1 (a mirror view, any other local player).
int SeatOf(const void* localPlayer) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (localPlayer != nullptr && g_seat[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed) == localPlayer)
        {
            return seat;
        }
    }
    return localPlayer != nullptr && localPlayer == g_playerOne.load(std::memory_order_relaxed) ? 0 : -1;
}

/// A further player is in (their local player followed).
bool AnyFurther() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_seat[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed) != nullptr)
        {
            return true;
        }
    }
    return false;
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

// Game thread only (GetViewPoint is called from Draw). By seat; index 0: player 1, a mirror view, any other.
std::array<ViewShape, kSeats> g_loggedShape{};
std::array<bool, kSeats>      g_shapeLogged{};

/// Logs the shape the first time each player is seen and whenever the bars' inputs change. The
/// FOV is logged with it but does not trigger a line, since the game's camera changes it often.
void LogShapeOnChange(int slot, const std::byte* viewInfo) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    ViewShape shape{};
    std::memcpy(&shape.aspectRatio, viewInfo + view_info::kAspectRatio, sizeof(shape.aspectRatio));
    shape.flags = static_cast<unsigned char>(*(viewInfo + view_info::kFlags)) & view_info::kKnownFlagBits;

    const auto at = static_cast<std::size_t>(slot);
    if (g_shapeLogged[at] && g_loggedShape[at] == shape)
    {
        return;
    }
    g_shapeLogged[at] = true;
    g_loggedShape[at] = shape;

    float fov = 0.0f;
    std::memcpy(&fov, viewInfo + view_info::kFov, sizeof(fov));
    platform::LogDebug(L"{}'s view shape (engine): constrain aspect ratio = {} (flag bits 0x{:X}), aspect ratio {:.4f}, "
                       L"FOV {:.1f}",
                       players::SeatName(slot), (shape.flags & view_info::kConstrainAspectRatioBit) != 0,
                       static_cast<unsigned>(shape.flags), shape.aspectRatio, fov);
}

// ---------------------------------------------------------------------------------------------
// Split players fill their half
// ---------------------------------------------------------------------------------------------
//
// While player 2 is in, every player's view takes the mod's FOV rule
// (ULocalPlayer::AspectRatioAxisConstraint), read by GetProjectionData after this function returns
// (0x140CC0B1D) for a view without the 16:9 constraint. The constraint itself is the main camera's
// own setting, cleared once per camera through the game's setter (player_two/camera/main_camera_shape.h).
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

/// The split FOV's log, per seat (index 0: player 1, a mirror view): at once for a new weight, half shape or camera
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
static_assert(kSeats == 3, "one record per seat below");
std::array<platform::SettledLog<FovSteady, int>, kSeats> g_fovLog = {platform::SettledLog<FovSteady, int>(kFovStillFrames),
                                                                     platform::SettledLog<FovSteady, int>(kFovStillFrames),
                                                                     platform::SettledLog<FovSteady, int>(kFovStillFrames)};

/// Each player's view width against the camera's own (tan of the half-angles; 1 without a split FOV), for the
/// camera code that places things across the view (player_two/camera/shoulder_scale.h). By seat.
std::array<std::atomic<float>, kSeats> g_widthRatio{1.0f, 1.0f, 1.0f};

/// With a weight chosen and the halves' shape known, writes the split FOV (views/view_shape.h,
/// SplitFov) under the MaintainXFOV rule; otherwise leaves the FOV to the MajorAxisFOV rule. `slot`: the seat (0 for
/// player 1, a mirror view).
void ApplySplitFov(void* localPlayer, std::byte* viewInfo, int slot) noexcept
{
    const auto at = static_cast<std::size_t>(slot);
    const float     weight = g_fovWeight.load(std::memory_order_relaxed);
    const HalfShape half{g_halfAspect.load(std::memory_order_relaxed), g_halfHeightRatio.load(std::memory_order_relaxed)};
    if (weight < 0.0f || half.aspect <= 0.0f || half.heightRatio <= 0.0f)
    {
        ApplyAxisRule(localPlayer, kMajorAxisFov);
        g_widthRatio[at].store(1.0f, std::memory_order_relaxed);
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
    g_widthRatio[at].store(WidthRatio(split, fov), std::memory_order_relaxed);

    const FovSteady steady{weight, half.aspect, half.heightRatio, cameraRatio};
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_fovLog[at].Reset();  // off: the next line starts fresh
    }
    else if (g_fovLog[at].Offer(steady, static_cast<int>(std::lround(fov * 10.0f))))
    {
        const float vertical = VerticalFovDegrees(split, half.aspect);
        platform::LogDebug(L"split FOV ({}): weight {:.2f}, half aspect {:.3f} (height {:.2f} of the screen): camera "
                           L"{:.1f} deg wide at {:.3f} -> {:.1f} deg wide, {:.1f} deg tall",
                           players::SeatName(slot), weight, half.aspect, half.heightRatio, fov,
                           cameraRatio, split, vertical);
    }
}

void ShapeSplitView(void* localPlayer, std::byte* viewInfo, int slot) noexcept
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
    ApplySplitFov(localPlayer, viewInfo, slot);
    if (!g_fillLogged)
    {
        g_fillLogged = true;
        platform::Log(L"split views: the FOV rule is the mod's{} (the halves fill by each main camera's own constraint "
                      L"setting, player_two/main_camera_shape)",
                      g_axisOffset.load(std::memory_order_relaxed) == 0 ? L" NOT: offset unconfirmed" : L"");
    }
}

/// Puts back the FOV rule of every player the mod changed, except the `removedPlayers`, which the
/// engine has already let go of and must not be touched, then forgets every record: this runs
/// when a further player changes, and the next split frame records the originals afresh.
void RestoreAxisRules(std::span<const void* const> removedPlayers) noexcept
{
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    g_axisRecords.Restore(
        removedPlayers,
        [offset](const void* player) -> std::uint8_t* {
            return offset != 0 ? static_cast<std::uint8_t*>(const_cast<void*>(player)) + offset : nullptr;
        },
        &IsModRule,
        [](std::uint8_t original) { platform::Log(L"a player's FOV rule restored to {}", static_cast<unsigned>(original)); });
    g_fillLogged = false;
}

/// The engine's view point, then the camera choice and the split's shape. `seat`: SeatOf(localPlayer).
void GetViewPointShaped(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass, int seat)
{
    const void* playerOne = g_playerOne.load(std::memory_order_relaxed);
    const bool  further   = seat >= 1;
    const bool  ownCamera = further && g_ownCamera[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed) &&
                           !g_onPlayerOne.load(std::memory_order_relaxed);

    // Until a further player's own camera is chosen, and while the columns hold a single-view moment, the further
    // player's view shows player 1's camera.
    void* source = further && !ownCamera && playerOne != nullptr ? const_cast<void*>(playerOne) : localPlayer;
    g_engineGetViewPoint(source, viewInfo, stereoPass);
    if (seat < 0)
    {
        // A mirror view (or any other local player): exactly the engine's result. Nothing is recorded for it, so
        // nothing outlives it (its FOV rule is never changed; a record would be restored into a freed player).
        return;
    }

    const int slot = seat;
    LogShapeOnChange(slot, viewInfo);
    if (!AnyFurther() || !g_splitShown.load(std::memory_order_relaxed))
    {
        return;  // one player, or player 1's single view: exactly the engine's result
    }
    if (g_fillOn.load(std::memory_order_relaxed))
    {
        ShapeSplitView(localPlayer, viewInfo, slot);
    }
    if (further && g_describePending[static_cast<std::size_t>(seat)].exchange(false, std::memory_order_relaxed))
    {
        DescribeViewPoint(seat, viewInfo, ownCamera);
    }
}

// Each player's camera as it was last drawn (by seat), for the streaming views (views/stream_views.h). Game thread
// (Draw).
std::array<std::optional<StreamCamera>, kSeats> g_rendered;

/// Same signature as ULocalPlayer::GetViewPoint.
void GetViewPoint(void* localPlayer, std::byte* viewInfo, std::int32_t stereoPass)
{
    const int seat = SeatOf(localPlayer);
    GetViewPointShaped(localPlayer, viewInfo, stereoPass, seat);
    if (seat >= 0 && AnyFurther())
    {
        g_rendered[static_cast<std::size_t>(seat)] = CameraOf(viewInfo);
    }
}

}  // namespace

bool InstallPlayerViewPoint(const platform::GameImage& image) noexcept
{
    const auto entry    = image.Rebase(facts::kGetViewPointEntry, sizeof(void*));
    const auto engineFn = image.Rebase(facts::kGetViewPoint.address);
    const auto vtable   = image.Rebase(facts::kLocalPlayerVTable);
    if (entry && engineFn && vtable)
    {
        g_engineGetViewPoint = reinterpret_cast<GetViewPointFn>(*engineFn);
        g_localPlayerVTable  = *vtable;
        g_installed = g_entry.Replace(*entry, *engineFn, reinterpret_cast<std::uintptr_t>(&GetViewPoint));
    }
    if (!g_installed)
    {
        platform::Log(L"ULocalPlayer::GetViewPoint's entry does not hold the engine's function; filled halves and the "
                      L"camera choice are unavailable");
    }
    return g_installed;
}

bool ViewPointInstalled() noexcept
{
    return g_installed;
}

void RemovePlayerViewPoint() noexcept
{
    if (!g_entry.Restore())
    {
        platform::Log(L"could not restore ULocalPlayer's GetViewPoint entry (another mod changed it after us); "
                      L"the replacement stays in place");
    }
}

namespace {

/// A new further player in `seat` (or none; the caller has seen the change): the new player started on their own
/// camera with their next view point described. Returns the player the seat held before (null for none): the caller
/// puts the FOV rules back once, skipping every seat's removed player (two seats can leave in one frame).
const void* SetSeat(int seat, const void* localPlayer) noexcept
{
    const auto  at       = static_cast<std::size_t>(seat);
    const void* previous = g_seat[at].exchange(localPlayer, std::memory_order_relaxed);
    g_ownCamera[at].store(localPlayer != nullptr, std::memory_order_relaxed);  // each new player starts on their own camera
    g_describePending[at].store(localPlayer != nullptr, std::memory_order_relaxed);
    g_shapeLogged[at] = false;  // a new player's shape is logged afresh
    return previous;
}

// The frame's follow (mod.cpp's until 2026-10-22). Game thread.
HalfShapeWatch                                   g_halfShape;
std::array<const void*, kSeats>                  g_followedSeat{};  // the players as last followed
std::array<std::uint32_t, kSeats>                g_followedJoins{};

/// Keeps the players current, and describes the local players when a further player appears (a subclass with its own
/// vtable would bypass the camera choice). The frame's index 0 and unbuilt seats stay null (mod.cpp SplitFrameNow).
void FollowPlayers(const SplitFrame& frame) noexcept
{
    if (frame.seats == g_followedSeat && frame.joins == g_followedJoins)
    {
        return;
    }
    bool                                       joined = false;
    std::array<const void*, players::kMaxPlayers> removed{};  // every changed seat's previous player
    std::size_t                                removedCount = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        if (frame.seats[at] != g_followedSeat[at] || frame.joins[at] != g_followedJoins[at])
        {
            // A new player at the old one's address still starts fresh.
            if (const void* previous = SetSeat(seat, frame.seats[at]); previous != nullptr)
            {
                removed[removedCount++] = previous;
            }
            joined = joined || frame.seats[at] != nullptr;
        }
    }
    if (removedCount > 0)
    {
        // Once, skipping every removed player; same thread as the replacement, so no call is in flight.
        RestoreAxisRules(std::span<const void* const>(removed.data(), removedCount));
    }
    g_followedSeat  = frame.seats;
    g_followedJoins = frame.joins;
    const bool any  = AnyFurther();
    g_playerOne.store(any ? frame.playerOne : nullptr, std::memory_order_relaxed);
    for (auto& rendered : g_rendered)
    {
        rendered.reset();
    }
    // The FOV rule's offset, confirmed by reflection before the first split frame uses it.
    if (joined)
    {
        const auto axisOffset = players::split_settings::AspectRatioAxisConstraintOffset();
        g_axisOffset.store(axisOffset.value_or(0), std::memory_order_relaxed);
        if (!axisOffset)
        {
            platform::Log(L"ULocalPlayer::AspectRatioAxisConstraint not found where the engine reads it; split views "
                          L"keep the game's FOV rule");
        }
    }
    if (!joined || !g_installed)
    {
        return;
    }
    auto* players = players::unreal::LocalPlayers(players::unreal::GameInstance());
    for (std::int32_t i = 0; players != nullptr && i < players->Num(); ++i)
    {
        auto* player = (*players)[i];
        platform::Log(L"local player {}: class {}, view point {}", i + 1, player->GetClassPrivate()->GetName(),
                      UsesEngineViewPoint(player) ? L"through the engine's ULocalPlayer entry"
                                                  : L"through ANOTHER vtable (the camera choice cannot reach it)");
    }
}

void SetHalfShape(const HalfShape& half) noexcept
{
    const bool known = half.aspect > 0.0f && half.heightRatio > 0.0f;
    g_halfAspect.store(known ? half.aspect : 0.0f, std::memory_order_relaxed);
    g_halfHeightRatio.store(known ? half.heightRatio : 0.0f, std::memory_order_relaxed);
}

/// Keeps the halves' shape current while the split is shown: the viewport size (it changes with the window) and
/// the layout (it changes by key or script). Read twice a second; until the first reading the split FOV leaves the
/// stock rule in place.
void FollowHalfShape(const SplitFrame& frame) noexcept
{
    // Three columns: the halves rule does not apply (stock FOV).
    switch (g_halfShape.Next(frame.splitShown, frame.columns > 2))
    {
        case HalfShapeWatch::Step::Reset: SetHalfShape({}); return;
        case HalfShapeWatch::Step::Skip: return;
        case HalfShapeWatch::Step::Read: break;
    }
    const platform::frame_watch::Job job(L"half shape read");
    auto*      playerOne  = static_cast<RC::Unreal::UObject*>(const_cast<void*>(frame.playerOne));
    auto**     controller = players::unreal::ControllerField(playerOne);
    const auto size   = controller != nullptr ? players::unreal::ViewportSizeOf(*controller) : std::nullopt;
    const auto layout = players::split_settings::TwoPlayerLayout();
    const bool sideBySide = layout && *layout == static_cast<std::uint8_t>(players::SplitLayoutKind::SideBySide);
    const auto half       = size && layout ? HalfOf(size->width, size->height, sideBySide) : HalfShape{};
    SetHalfShape(half);
    if (g_halfShape.AspectChanged(half.aspect))
    {
        if (half.aspect > 0.0f)
        {
            platform::Log(L"split halves: viewport {}x{}, layout {}, half aspect {:.3f}, height {:.2f} of the screen",
                          size->width, size->height, *layout, half.aspect, half.heightRatio);
        }
        else
        {
            platform::Log(L"split halves: the viewport size or layout cannot be read; the split FOV waits (stock rule "
                          L"meanwhile)");
        }
    }
}

}  // namespace

void Follow(const SplitFrame& frame) noexcept
{
    FollowPlayers(frame);
    g_splitShown.store(frame.splitShown, std::memory_order_relaxed);
    g_viewsSplit.store(frame.viewsSplit, std::memory_order_relaxed);
    if (g_onPlayerOne.exchange(frame.onPlayerOne, std::memory_order_relaxed) != frame.onPlayerOne)
    {
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            g_describePending[static_cast<std::size_t>(seat)].store(true, std::memory_order_relaxed);
        }
    }
    FollowHalfShape(frame);
}

bool ToggleOwnCamera() noexcept
{
    // Every further player's view together, to the opposite of the first one in.
    const bool own = !ShowsOwnCamera();
    bool       any = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        const bool in = g_seat[at].load(std::memory_order_relaxed) != nullptr;
        g_ownCamera[at].store(in && own, std::memory_order_relaxed);
        if (in)
        {
            g_describePending[at].store(true, std::memory_order_relaxed);
            any = true;
        }
    }
    return any && own;
}

bool ShowsOwnCamera() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        if (g_seat[at].load(std::memory_order_relaxed) != nullptr)
        {
            return g_ownCamera[at].load(std::memory_order_relaxed);  // the first further player in
        }
    }
    return false;
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

std::optional<StreamCamera> RenderedCamera(int seat) noexcept
{
    return seat >= 0 && seat < players::kMaxPlayers ? g_rendered[static_cast<std::size_t>(seat)] : std::nullopt;
}

bool SplitShown() noexcept
{
    return g_splitShown.load(std::memory_order_relaxed);
}

bool ViewsSplit() noexcept
{
    return g_viewsSplit.load(std::memory_order_relaxed);
}

bool FillsHalves() noexcept
{
    return g_installed && g_fillOn.load(std::memory_order_relaxed);  // without the entry nothing shapes the halves
}

void SetSplitFovWeight(float weight) noexcept
{
    g_fovWeight.store(weight >= 0.0f && weight <= 1.0f ? weight : -1.0f, std::memory_order_relaxed);
}

float SplitFovWeight() noexcept
{
    return g_fovWeight.load(std::memory_order_relaxed);
}

float SplitWidthRatio(int seat) noexcept
{
    return seat >= 0 && seat < players::kMaxPlayers ? g_widthRatio[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed)
                                                    : 1.0f;
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

#if LSS_DEV

void RegisterViewPointCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name     = L"split-fov",
                       .shape    = control::Shape::Float,
                       .minFloat = 0.0,
                       .maxFloat = 1.0,
                       .words    = {L"native"},
                       .help     = L"filled halves' FOV: 0 keeps the single-player framing, 1 its on-screen scale (0.5 "
                                   L"at start; views/view_shape.h); native = the stock MajorAxisFOV rule",
                       .set      = [](const Value& v) {
                           if (!ViewPointInstalled())
                           {
                               return Outcome{false, L"the view-point entry is not installed"};
                           }
                           const bool  native = std::holds_alternative<std::wstring>(v);
                           const float blend  = native ? -1.0f : static_cast<float>(std::get<double>(v));
                           SetSplitFovWeight(blend);
                           // The resulting FOV is logged here when a filled split view first uses it.
                           return Outcome{true, native ? L"stock MajorAxisFOV rule"
                                                       : L"weight set; applies to filled halves"};
                       },
                       .get = [] {
                           const float weight = SplitFovWeight();
                           return weight < 0.0f ? Value{std::wstring(L"native")}
                                                : Value{control::FloatReading(weight)};
                       }});
}

#endif

}  // namespace lss::views
