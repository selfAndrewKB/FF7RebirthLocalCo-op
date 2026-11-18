#include "views/player_view_point.h"

#include "facts/engine.h"
#include "platform/frame_watch.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/settled_log.h"
#include "platform/vtable_entry.h"
#include "players/console_access.h"
#include "players/split_settings_access.h"
#include "players/unreal_access.h"
#include "views/axis_records.h"
#include "views/minimal_view_info.h"
#include "views/split_fov_rules.h"
#include "views/stream_view_math.h"
#include "views/view_shape.h"
#include "views/window_span.h"

#include "platform/command_registry.h"

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
std::atomic<const void*>   g_playerOne{nullptr};  // player 1's ULocalPlayer as the frame names it (null before the first frame)
std::array<std::atomic<const void*>, kSeats> g_seat{};          // each seat's ULocalPlayer (index 0 unused)
std::array<std::atomic<bool>, kSeats>        g_ownCamera{};     // the seat's view shows its own camera
std::array<std::atomic<bool>, kSeats>        g_describePending{};
std::array<std::atomic<bool>, kSeats>        g_onPlayerOne{};     // the seat's column on player 1's camera (column_source)
std::atomic<bool>          g_fillOn{true};
std::atomic<bool>          g_splitShown{false};  // a seat in and its own view on screen
std::atomic<bool>          g_viewsSplit{false};  // more than one view on screen (the HUD per column)
std::atomic<std::uint32_t> g_axisOffset{0};  // 0 until reflection has confirmed it
std::atomic<float>         g_fovWeight{split_fov_rules::kDefaultWeight};  // 0..1; negative: the MajorAxisFOV rule
std::atomic<float>         g_narrowWeight{split_fov_rules::kDefaultNarrowWeight};  // 0..1: a narrower full-height view's
std::atomic<std::int32_t>  g_viewportWidth{0};   // the whole game viewport in pixels (every view's part together); 0 until known
std::atomic<std::int32_t>  g_viewportHeight{0};

/// A seat's view point, the first time it is computed from a given camera.
void DescribeViewPoint(int seat, const std::byte* viewInfo, bool ownCamera) noexcept
{
    float yaw = 0.0f;
    std::memcpy(&yaw, viewInfo + view_info::kYaw, sizeof(yaw));
    const auto previousSet = static_cast<unsigned>(*(viewInfo + view_info::kPreviousViewTransformSet));
    platform::LogDebug(L"{}'s view point from {}: yaw {:.1f}; PreviousViewTransform set = {}", players::SeatName(seat),
                       ownCamera ? L"their own camera" : L"player 1's camera", yaw, previousSet);
}

/// `localPlayer`'s seat (0 player 1's), or -1 (a mirror view, any other local player).
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

/// A seat is in (its local player followed).
bool AnySeat() noexcept
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

// Game thread only (GetViewPoint is called from Draw). By seat; index 0: player 1.
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
// While a seat is in, every player's view takes the mod's FOV rule
// (ULocalPlayer::AspectRatioAxisConstraint), read by GetProjectionData after this function returns
// for a view without the 16:9 constraint. The constraint itself is the main camera's
// own setting, cleared once per camera through the game's setter (camera/main_camera_shape.h).
//
// The FOV rule is written once per player, at the player's first split frame (and again only if
// the mod's own FOV mode changes), then left alone while a scene may be at work: the game's only
// writer, the level-sequence camera cut, saves the value before it writes its own and puts that
// value back when the cut returns to the previous view target (research/RE-p2-hud-view-levers.md). A
// value the game holds meanwhile is logged, not overwritten. A scene that ends without that
// return leaves the game's rule in place (seen live: a skippable field scene left player 1's
// column on MaintainXFOV, the picture zoomed out), so when the split comes back after player 1's
// single view (every scene takes it) the rule is written again where the game left another value,
// once, logged. The same when the fill is switched on again.
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
/// unconfirmed or there is no room to remember the original, in which case nothing was changed). `slot`: the seat,
/// for the log.
bool ApplyAxisRule(void* localPlayer, std::uint8_t rule, int slot) noexcept
{
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    if (offset == 0)
    {
        return false;  // not confirmed by reflection: the engine's rule stays
    }
    auto*              field  = static_cast<std::uint8_t*>(localPlayer) + offset;
    const std::uint8_t before = *field;
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
        case AxisApplied::Retaken:  // the game's writer (a scene's camera cut) is done and left its value
            platform::Log(L"{}'s FOV rule set back to the mod's {} (the game had left {} in place)",
                          players::SeatName(slot), static_cast<unsigned>(*field), static_cast<unsigned>(before));
            break;
        case AxisApplied::First:
        case AxisApplied::RuleChanged:
        case AxisApplied::Unchanged: break;
    }
    return true;
}

/// The mirror views' FOV rule (a mirror is the mod's own local player, views/mirror_player.h): written where it
/// differs, with no record and nothing put back, since the mod removes the player itself and a record would be
/// restored into a freed player. For a seat's player (`slot` 0 for player 1, a seat) the recorded rule above.
bool SetAxisRule(void* localPlayer, std::uint8_t rule, int slot) noexcept
{
    if (slot >= 0)
    {
        return ApplyAxisRule(localPlayer, rule, slot);
    }
    const auto offset = g_axisOffset.load(std::memory_order_relaxed);
    if (offset == 0)
    {
        return false;
    }
    auto* field = static_cast<std::uint8_t*>(localPlayer) + offset;
    if (*field != rule)
    {
        *field = rule;
        platform::LogDebug(L"a mirror view's FOV rule set to the mod's {}", static_cast<unsigned>(rule));
    }
    return true;
}

/// The split FOV's log, per seat (index 0: player 1): at once for a new weight, view shape or camera aspect ratio;
/// for a new camera FOV once it has held still (to 0.1 degree) for about half a second, so a zoom or a framing blend
/// logs where it ends instead of every frame.
struct FovSteady
{
    float weight       = -2.0f;
    float width        = 0.0f;  // the view and the window, in pixels
    float height       = 0.0f;
    float windowWidth  = 0.0f;
    float windowHeight = 0.0f;
    float cameraRatio  = 0.0f;
    bool  operator==(const FovSteady&) const = default;
};
constexpr std::uint32_t kFovStillFrames = 30;
static_assert(kSeats == 3, "one record per seat below");
std::array<platform::SettledLog<FovSteady, int>, kSeats> g_fovLog = {platform::SettledLog<FovSteady, int>(kFovStillFrames),
                                                                     platform::SettledLog<FovSteady, int>(kFovStillFrames),
                                                                     platform::SettledLog<FovSteady, int>(kFovStillFrames)};

/// Each player's view width against the camera's own (tan of the half-angles; 1 without a split FOV), for the
/// camera code that places things across the view (camera/shoulder_scale.h). By seat.
std::array<std::atomic<float>, kSeats> g_widthRatio{1.0f, 1.0f, 1.0f};

/// Each player's view's width over its height as last shaped (0 until known), for the streaming views. By seat.
std::array<std::atomic<float>, kSeats> g_viewAspect{};

/// The view's part of the window and the window, in pixels, as GetProjectionData sizes them: the player's Size
/// fractions (facts::kLocalPlayerSize, written by the engine's layout pass every redraw) times the viewport's pixels
/// (read twice a second, FollowViewportSize). A view of a span is measured against its own monitor. False until the
/// viewport's size is known, or when the fractions cannot be read or are not a view's.
bool ReadShape(const void* localPlayer, split_fov_rules::Shape& shape) noexcept
{
    const std::int32_t width  = g_viewportWidth.load(std::memory_order_relaxed);
    const std::int32_t height = g_viewportHeight.load(std::memory_order_relaxed);
    float              size[2]{};
    if (width <= 0 || height <= 0 ||
        !platform::CopyGuarded(size, static_cast<const std::uint8_t*>(localPlayer) + facts::kLocalPlayerSize, sizeof(size)) ||
        !(size[0] > 0.0f) || !(size[1] > 0.0f) || size[0] > 1.0f || size[1] > 1.0f)
    {
        return false;
    }
    shape.windowWidth  = static_cast<float>(width);
    shape.windowHeight = static_cast<float>(height);
    shape.viewWidth    = std::trunc(size[0] * shape.windowWidth);  // as GetProjectionData sizes the view (TruncToInt)
    shape.viewHeight   = std::trunc(size[1] * shape.windowHeight);
    if (window_span::In())
    {
        shape = split_fov_rules::OnItsMonitor(shape);
    }
    return shape.viewWidth > 0.0f && shape.viewHeight > 0.0f;
}

/// What a view's last call decided, for the Info line on each change: one record per local player the engine allows
/// (a mirror view's included). Game thread (the view point is called from Draw).
struct Seen
{
    const void*   player   = nullptr;
    std::uint64_t lastSeen = 0;  // the note counter at this view's last call (the least recently seen is reused)
    int           width    = 0;  // the view and the window, in pixels
    int           height   = 0;
    int           windowW  = 0;
    int           windowH  = 0;
    float         weight   = -1.0f;  // the weight the rule took (a new one logs again)
    bool          applies  = false;
};
constexpr std::size_t kSeenRecords = 4;  // one per local player the engine allows
std::array<Seen, kSeenRecords> g_seen;
std::uint64_t                  g_notes = 0;  // counts the notes, for the records' last-seen order

/// The view's record, or (with `create`) a new one in an empty or the least recently seen record; null otherwise.
Seen* RecordOf(const void* player, bool create) noexcept
{
    ++g_notes;
    Seen* oldest = &g_seen[0];
    for (Seen& seen : g_seen)
    {
        if (seen.player == player)
        {
            seen.lastSeen = g_notes;
            return &seen;
        }
        if (seen.lastSeen < oldest->lastSeen)
        {
            oldest = &seen;  // an empty record has lastSeen 0, so it is taken first
        }
    }
    if (!create)
    {
        return nullptr;
    }
    *oldest          = {};
    oldest->player   = player;
    oldest->lastSeen = g_notes;
    return oldest;
}

constexpr float kWidenedWindowAspect = 1.8f;   // wider than the game's 16:9 (1.778)
constexpr float kAspectMatch         = 0.01f;  // a camera aspect within this of the window's is the window's

/// The Info line when a view starts or stops taking a rule, or changes shape while taking it.
void NoteShape(const void* localPlayer, int slot, const split_fov_rules::Shape& shape, bool wide, bool narrow, float aspect, float fov,
               float split) noexcept
{
    const bool applies = wide || narrow;
    Seen*      seen    = RecordOf(localPlayer, applies);  // a view that never takes a rule holds no record
    if (seen == nullptr)
    {
        return;
    }
    const int width = static_cast<int>(shape.viewWidth), height = static_cast<int>(shape.viewHeight);
    const int windowW = static_cast<int>(shape.windowWidth), windowH = static_cast<int>(shape.windowHeight);
    const float weight = narrow ? g_narrowWeight.load(std::memory_order_relaxed) : g_fovWeight.load(std::memory_order_relaxed);
    if (seen->applies == applies &&
        (!applies || (seen->width == width && seen->height == height && seen->windowW == windowW && seen->windowH == windowH &&
                      seen->weight == weight)))
    {
        return;
    }
    const wchar_t* who = slot >= 0 ? players::SeatName(slot) : L"a mirror view";
    // The game's cameras report 16:9 and the game shows a wider window with bars; a camera reporting a wider window's
    // own shape has been widened by something else (an ultrawide tool), which the split FOV then scales again.
    const float        windowAspect = windowH > 0 ? static_cast<float>(windowW) / static_cast<float>(windowH) : 0.0f;
    const bool         widened      = windowAspect > kWidenedWindowAspect && std::fabs(aspect - windowAspect) < kAspectMatch;
    const std::wstring note =
        widened ? L" (the camera reports the window's own shape: an ultrawide tool may be widening it)" : L"";
    if (narrow)
    {
        platform::Log(L"split FOV: {}'s {}x{} view of a {}x{} window takes the narrow view's FOV (width {:.2f} of the single-player "
                      L"picture, weight {:.2f}): the camera's {:.1f} deg at {:.3f} becomes {:.1f} deg wide{}",
                      who, width, height, windowW, windowH, split_fov_rules::WidthRatio(shape, aspect),
                      g_narrowWeight.load(std::memory_order_relaxed), fov, aspect, split, note);
    }
    else if (wide)
    {
        platform::Log(L"split FOV: {}'s {}x{} view of a {}x{} window takes the split FOV (height {:.2f} of the single-player picture, "
                      L"weight {:.2f}): the camera's {:.1f} deg at {:.3f} becomes {:.1f} deg wide{}",
                      who, width, height, windowW, windowH, split_fov_rules::HeightRatio(shape, aspect),
                      g_fovWeight.load(std::memory_order_relaxed), fov, aspect, split, note);
    }
    else
    {
        platform::Log(L"split FOV: {}'s {}x{} view of a {}x{} window keeps the camera's FOV (the shape of the camera's image){}", who,
                      width, height, windowW, windowH, note);
    }
    seen->applies = applies;
    seen->width   = width;
    seen->height  = height;
    seen->windowW = windowW;
    seen->windowH = windowH;
    seen->weight  = weight;
}

/// With a weight chosen and the view's shape known, writes the MaintainXFOV rule and the view's FOV
/// (views/split_fov_rules.h): a view wider than the camera's image takes the split FOV, which on a full-height column
/// of a wider monitor keeps the camera's vertical FOV; a full-height view narrower than the image takes the narrow
/// view's FOV at the split-fov.vertical weight (1.00 keeps the single-player scale); a view of the image's shape keeps the
/// camera's FOV.
/// Otherwise the MajorAxisFOV rule. `slot`: the seat (0 for player 1), or -1 for a mirror view.
void ApplySplitFov(void* localPlayer, std::byte* viewInfo, int slot) noexcept
{
    const float            weight = g_fovWeight.load(std::memory_order_relaxed);
    split_fov_rules::Shape shape;
    const bool             known = ReadShape(localPlayer, shape);
    if (slot >= 0)
    {
        g_viewAspect[static_cast<std::size_t>(slot)].store(known ? shape.viewWidth / shape.viewHeight : 0.0f, std::memory_order_relaxed);
    }
    if (weight < 0.0f || !known)
    {
        SetAxisRule(localPlayer, kMajorAxisFov, slot);
        if (slot >= 0)
        {
            g_widthRatio[static_cast<std::size_t>(slot)].store(1.0f, std::memory_order_relaxed);
        }
        return;
    }
    if (!SetAxisRule(localPlayer, kMaintainXFov, slot))
    {
        return;  // the rule could not be set, so a written FOV would be read under the wrong rule
    }
    float fov         = 0.0f;
    float cameraRatio = 0.0f;
    std::memcpy(&fov, viewInfo + view_info::kFov, sizeof(fov));
    std::memcpy(&cameraRatio, viewInfo + view_info::kAspectRatio, sizeof(cameraRatio));
    const bool  wide   = split_fov_rules::Applies(shape, cameraRatio);
    const bool  narrow = split_fov_rules::AppliesNarrow(shape, cameraRatio);
    const float split  = wide     ? split_fov_rules::SplitFov(fov, cameraRatio, shape, weight)
                         : narrow ? split_fov_rules::NarrowFov(fov, cameraRatio, shape, g_narrowWeight.load(std::memory_order_relaxed))
                                  : fov;
    std::memcpy(viewInfo + view_info::kFov, &split, sizeof(split));
    NoteShape(localPlayer, slot, shape, wide, narrow, cameraRatio, fov, split);
    if (slot < 0)
    {
        return;
    }
    const auto at = static_cast<std::size_t>(slot);
    g_widthRatio[at].store(WidthRatio(split, fov), std::memory_order_relaxed);

    const FovSteady steady{weight, shape.viewWidth, shape.viewHeight, shape.windowWidth, shape.windowHeight, cameraRatio};
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_fovLog[at].Reset();  // off: the next line starts fresh
    }
    else if (g_fovLog[at].Offer(steady, static_cast<int>(std::lround(fov * 10.0f))))
    {
        const float vertical = VerticalFovDegrees(split, shape.viewWidth / shape.viewHeight);
        platform::LogDebug(L"split FOV ({}): weight {:.2f}, view {:.0f}x{:.0f} of {:.0f}x{:.0f}: camera {:.1f} deg wide at {:.3f} -> "
                           L"{:.1f} deg wide, {:.1f} deg tall",
                           players::SeatName(slot), weight, shape.viewWidth, shape.viewHeight, shape.windowWidth, shape.windowHeight,
                           fov, cameraRatio, split, vertical);
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
                      L"setting, camera/main_camera_shape)",
                      g_axisOffset.load(std::memory_order_relaxed) == 0 ? L" NOT: offset unconfirmed" : L"");
    }
}

/// Puts back the FOV rule of every player the mod changed, except the `removedPlayers`, which the
/// engine has already let go of and must not be touched, then forgets every record: this runs
/// when a seat's player changes, and the next split frame records the originals afresh.
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
    const bool  isSeat    = seat >= 1;
    const bool  ownCamera = isSeat && g_ownCamera[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed) &&
                           !g_onPlayerOne[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);

    // Until a seat's own camera is chosen, and while its column shows player 1's picture
    // (views/column_source.h), the seat's view shows player 1's camera.
    void* source = isSeat && !ownCamera && playerOne != nullptr ? const_cast<void*>(playerOne) : localPlayer;
    g_engineGetViewPoint(source, viewInfo, stereoPass);
    const bool views = g_splitShown.load(std::memory_order_relaxed) || g_viewsSplit.load(std::memory_order_relaxed);
    if (seat < 0)
    {
        // A mirror view (or any other local player): the engine's result, shaped for its own column while several
        // views fill their columns (its monitor may be of another shape than player 1's). Nothing else is recorded
        // for it (SetAxisRule), so nothing outlives it.
        if (views && g_fillOn.load(std::memory_order_relaxed))
        {
            ShapeSplitView(localPlayer, viewInfo, -1);
        }
        return;
    }

    const int slot = seat;
    LogShapeOnChange(slot, viewInfo);
    if (!views)
    {
        return;  // one view on screen (one player, or player 1's single view): exactly the engine's result
    }
    if (g_fillOn.load(std::memory_order_relaxed))
    {
        ShapeSplitView(localPlayer, viewInfo, slot);
    }
    if (isSeat && g_describePending[static_cast<std::size_t>(seat)].exchange(false, std::memory_order_relaxed))
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
    if (seat >= 0 && AnySeat())
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
        return false;
    }
    // The settings file's weight (one of the table's words, so it maps), as the console sets it later.
    namespace settings = platform::settings;
    SetSplitFovWeight(SplitFovWeightOf(settings::Of(settings::kHorizontalFov).word));
    g_narrowWeight.store(SplitFovWeightOf(settings::Of(settings::kVerticalFov).word, split_fov_rules::kDefaultNarrowWeight), std::memory_order_relaxed);
    platform::Log(L"split FOV: vertical split weight {} ({})", settings::Of(settings::kVerticalFov).word,
                  settings::Why(settings::kVerticalFov));
    platform::Log(L"split FOV: horizontal split weight {} ({})", settings::Of(settings::kHorizontalFov).word,
                  settings::Why(settings::kHorizontalFov));
    return true;
}

std::wstring SplitFovWord()
{
    return SplitFovWordOf(SplitFovWeight());
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

/// A new player in `seat` (or none; the caller has seen the change): the new player started on their own
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

// The frame's follow. Game thread.
ViewportSizeWatch                                g_viewportSize;
std::array<const void*, kSeats>                  g_followedSeat{};  // the players as last followed
std::array<std::uint32_t, kSeats>                g_followedJoins{};

/// The FOV rule's offset, confirmed by reflection before the first shaped view uses it: at a seat's join, and when
/// several views first show (the mirror views' columns with no seat in). Logged once if not found; tried again at
/// the next occasion.
void ConfirmAxisOffset() noexcept
{
    const auto axisOffset = players::split_settings::AspectRatioAxisConstraintOffset();
    g_axisOffset.store(axisOffset.value_or(0), std::memory_order_relaxed);
    if (!axisOffset)
    {
        platform::Log(L"ULocalPlayer::AspectRatioAxisConstraint not found where the engine reads it; split views "
                      L"keep the game's FOV rule");
    }
}

/// Keeps the players current, and describes the local players when a seat's player appears (a subclass with its own
/// vtable would bypass the camera choice). The frame's index 0 and unbuilt seats stay null (mod.cpp SplitFrameNow).
void FollowPlayers(const SplitFrame& frame) noexcept
{
    // Whoever is in: the mirror views' columns shape player 1's view too. A new player 1 (another local player object)
    // takes the old one's FOV record with it, so no restore writes through the old pointer.
    const void* previousOne = g_playerOne.exchange(frame.playerOne, std::memory_order_relaxed);
    if (previousOne != nullptr && previousOne != frame.playerOne)
    {
        const void* const gone[1] = {previousOne};
        RestoreAxisRules(gone);
    }
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
    for (auto& rendered : g_rendered)
    {
        rendered.reset();
    }
    if (joined)
    {
        ConfirmAxisOffset();
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

void SetViewportSize(std::int32_t width, std::int32_t height) noexcept
{
    const bool known = width > 0 && height > 0;
    g_viewportWidth.store(known ? width : 0, std::memory_order_relaxed);
    g_viewportHeight.store(known ? height : 0, std::memory_order_relaxed);
}

/// The size of the image the game's HUD is laid out in (the HUD layout's "UI image"), and whether the game set it. The
/// game's own rule, at each viewport resize, writes r.CompositeLayer.FixedWidth/FixedHeight: 1920x1080 for a viewport
/// 1080 high or less, otherwise 3840x2160 (research/RE-hud-wide-native-window.md). Any other size was set by something
/// else (an ultrawide tool), and the HUD is then laid out in an image of another shape than the game's.
std::int32_t g_uiImageHeight = 0;  // game thread: a viewport height whose UI image size is still to be read

void LogUiImageSource(std::int32_t viewportHeight) noexcept
{
    const auto width  = players::console::VariableInt(L"r.CompositeLayer.FixedWidth");
    const auto height = players::console::VariableInt(L"r.CompositeLayer.FixedHeight");
    const int  ruleW  = viewportHeight <= 1080 ? 1920 : 3840;
    const int  ruleH  = viewportHeight <= 1080 ? 1080 : 2160;
    if (!width || !height)
    {
        platform::Log(L"HUD layout: the UI image size cannot be read (r.CompositeLayer.FixedWidth/FixedHeight)");
        return;
    }
    if (*width == 0 || *height == 0)
    {
        platform::Log(L"HUD layout: the UI image size is unset (r.CompositeLayer.FixedWidth/FixedHeight 0); the game "
                      L"sizes it from its saved window width");
        return;
    }
    if (*width == ruleW && *height == ruleH)
    {
        platform::Log(L"HUD layout: the UI image size {}x{} is the game's own rule for a {}-high viewport", *width,
                      *height, viewportHeight);
        return;
    }
    platform::LogWarning(L"HUD layout: the UI image size {}x{} is set from outside the game "
                         L"(r.CompositeLayer.FixedWidth/FixedHeight; the game's own rule for a {}-high viewport gives "
                         L"{}x{}): another tool is changing the HUD's layout",
                         *width, *height, viewportHeight, ruleW, ruleH);
}

/// Keeps the viewport's size current while several views show (it changes with the window): read twice a second;
/// each view's own shape is its fractions of it, read at each view point. Until the first reading the split FOV
/// leaves the stock rule in place.
void FollowViewportSize(const SplitFrame& frame) noexcept
{
    switch (g_viewportSize.Next(frame.splitShown || frame.viewsSplit))
    {
        case ViewportSizeWatch::Step::Reset: SetViewportSize(0, 0); return;
        case ViewportSizeWatch::Step::Skip: return;
        case ViewportSizeWatch::Step::Read: break;
    }
    const platform::frame_watch::Job job(L"viewport size read");
    auto*      playerOne  = static_cast<RC::Unreal::UObject*>(const_cast<void*>(frame.playerOne));
    auto**     controller = players::unreal::ControllerField(playerOne);
    const auto size       = controller != nullptr ? players::unreal::ViewportSizeOf(*controller) : std::nullopt;
    SetViewportSize(size ? size->width : 0, size ? size->height : 0);
    // The UI image's size is read one reading after a size change: the game's own rule runs when it handles the
    // resize, which may come after the mod first sees the new size.
    if (g_uiImageHeight != 0 && size && size->height == g_uiImageHeight)
    {
        LogUiImageSource(g_uiImageHeight);
        g_uiImageHeight = 0;
    }
    if (g_viewportSize.SizeChanged(size ? size->width : 0, size ? size->height : 0))
    {
        if (size)
        {
            platform::Log(L"split views: viewport {}x{}; each view's shape is its part of it", size->width, size->height);
            g_uiImageHeight = size->height;
        }
        else
        {
            platform::Log(L"split views: the viewport size cannot be read; the split FOV waits (stock rule meanwhile)");
        }
    }
}

}  // namespace

void Follow(const SplitFrame& frame) noexcept
{
    FollowPlayers(frame);
    if (!g_splitShown.exchange(frame.splitShown, std::memory_order_relaxed) && frame.splitShown)
    {
        g_axisRecords.RetakeAll();  // the single view is over: a scene's camera cut may have left the game's rule
    }
    const bool viewsWere = g_viewsSplit.exchange(frame.viewsSplit, std::memory_order_relaxed);
    if (!viewsWere && frame.viewsSplit)
    {
        g_axisRecords.RetakeAll();  // the same for the columns no player fills (the mirror views) coming back
        if (g_axisOffset.load(std::memory_order_relaxed) == 0)
        {
            ConfirmAxisOffset();  // no seat has joined yet: the mirror views' columns are the first shaped views
        }
    }
    else if (viewsWere && !frame.viewsSplit && !frame.splitShown && !AnySeat())
    {
        RestoreAxisRules({});  // one view again with no seat in: player 1's own rule back (a seat's leave does it otherwise)
        for (auto& aspect : g_viewAspect)
        {
            aspect.store(0.0f, std::memory_order_relaxed);  // one view: no shape of its own
        }
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto at = static_cast<std::size_t>(seat);
        if (g_onPlayerOne[at].exchange(frame.onPlayerOne[at], std::memory_order_relaxed) != frame.onPlayerOne[at])
        {
            g_describePending[at].store(true, std::memory_order_relaxed);  // the next view point's log line
        }
    }
    FollowViewportSize(frame);
}

bool ToggleOwnCamera() noexcept
{
    // Every seat's view together, to the opposite of the first one in.
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
            return g_ownCamera[at].load(std::memory_order_relaxed);  // the first seat in
        }
    }
    return false;
}

bool ToggleFill() noexcept
{
    const bool on = !g_fillOn.load(std::memory_order_relaxed);
    g_fillOn.store(on, std::memory_order_relaxed);
    g_fillLogged = false;
    if (on)
    {
        g_axisRecords.RetakeAll();  // a scene while the fill was off may have left the game's rule
    }
    return on;
}

float ViewAspectOf(const void* localPlayer) noexcept
{
    split_fov_rules::Shape shape;
    return localPlayer != nullptr && ReadShape(localPlayer, shape) && shape.viewHeight > 0.0f
               ? shape.viewWidth / shape.viewHeight
               : 0.0f;
}

float ViewAspect(int seat) noexcept
{
    return seat >= 0 && seat < players::kMaxPlayers ? g_viewAspect[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed)
                                                    : 0.0f;
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

bool OnPlayerOne(int seat) noexcept
{
    return players::SeatIndex(seat) && g_onPlayerOne[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
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

void RegisterViewPointOptions()
{
    using control::Outcome;
    // The option's command from its row (the words 0.00 to 1.00); the resulting FOV is logged when a filled split
    // view first uses the weight.
    control::RegisterOption(
        platform::settings::kHorizontalFov,
        [](const std::wstring& word) {
            if (!ViewPointInstalled())
            {
                return Outcome{false, L"the view-point entry is not installed"};
            }
            SetSplitFovWeight(SplitFovWeightOf(word));
            platform::Log(L"split FOV: horizontal split weight {} (the console)", word);
            return Outcome{true, L"weight set; applies to filled halves"};
        },
        [] { return SplitFovWord(); });
    control::RegisterOption(
        platform::settings::kVerticalFov,
        [](const std::wstring& word) {
            const float weight = SplitFovWeightOf(word, split_fov_rules::kDefaultNarrowWeight);
            if (!(weight >= 0.0f && weight <= 1.0f))
            {
                return Outcome{false, L"a weight 0.00 to 1.00"};  // the shape checks the words
            }
            g_narrowWeight.store(weight, std::memory_order_relaxed);
            platform::Log(L"split FOV: vertical split weight {} (the console)", word);
            return Outcome{true, L"weight set; applies to full-height views narrower than the game's image"};
        },
        [] { return SplitFovWordOf(g_narrowWeight.load(std::memory_order_relaxed)); });
}

#if LSS_DEV

void RegisterViewPointCommands()
{
    using control::Outcome;
    // The stock rule for a comparison: on puts the weight aside, off brings it back.
    static float aside = split_fov_rules::kDefaultWeight;
    control::Register({.name  = L"split-fov.native",
                       .shape = control::Shape::OnOff,
                       .help  = L"the stock MajorAxisFOV rule in place of the split-fov.horizontal weight, for a "
                                L"comparison; off brings the weight back",
                       .set   = [](const control::Value& v) {
                           if (!ViewPointInstalled())
                           {
                               return Outcome{false, L"the view-point entry is not installed"};
                           }
                           const bool  on     = std::get<bool>(v);
                           const float weight = SplitFovWeight();
                           if (on && weight >= 0.0f)
                           {
                               aside = weight;
                               SetSplitFovWeight(-1.0f);
                           }
                           else if (!on && weight < 0.0f)
                           {
                               SetSplitFovWeight(aside);
                           }
                           return Outcome{true, on ? L"stock MajorAxisFOV rule" : L"the split-fov.horizontal weight applies"};
                       },
                       .get = [] { return control::Value{SplitFovWeight() < 0.0f}; }});
}

#endif

}  // namespace lss::views
