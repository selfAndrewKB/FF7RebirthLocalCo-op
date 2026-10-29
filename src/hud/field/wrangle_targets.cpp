#include "hud/field/wrangle_targets.h"

#include "facts/hud.h"
#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::hud::wrangle_targets {

namespace {

// RE-chocobo-wrangle-targets.md §5, §6 (the offsets are the same in both builds).
constexpr std::uintptr_t kCaptureManager = 0x1490FEDC8;  // g_EndChocoboCaptureManager (data)
constexpr std::uint32_t  kController     = 0x18;  // -> the capture controller (ctl)
constexpr std::uint32_t  kEntries        = 664;   // ctl: the chocobo entries (data)
constexpr std::uint32_t  kEntryCount     = 672;   // ctl: their count (i32)
constexpr std::uint32_t  kEntrySize      = 392;
constexpr std::uint32_t  kEntryBubble    = 360;   // entry: the Status_00 bubble widget (weak)
constexpr std::uint32_t  kTargetWeak     = 192;   // ctl: the current target (weak); the reticle shows while it resolves
constexpr std::uint32_t  kReticleWeak    = 496;   // ctl: the Target_C widget (weak)
constexpr std::uint32_t  kTargetLocation = 792;   // ctl: the target's world location (FVector), the game's placer's
constexpr std::uint32_t  kTranslation    = 0x90;  // widget: RenderTransform.Translation x, y
constexpr std::size_t    kMaxEntries     = 16;

// The game's per-entry placer (the bubble, its small form and the arrow, from the entry's state byte) and the shared
// projection helper (a world point to the root overlay, with the edge clamp and the arrow's angle), §2 and §6.
constexpr std::uint8_t kPlaceEntryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55,
                                             0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlaceEntry{L"ChocoboCapture__PlaceStatusIconAndArrow_PrimaryPC", 0x14357CB40,
                                             kPlaceEntryBytes};
constexpr std::uint8_t kProjectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
                                          0x57, 0x41, 0x56, 0xB8, 0xC0, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kProject{L"EndUI__ProjectWorldToRootOverlay_PrimaryPC_EdgeAngle", 0x143BAD5E8,
                                          kProjectBytes};
/// The edge margin the build's own placers pass the helper (read at their call sites: 1.004 0.0, 1.005 50.0).
constexpr platform::PerBuild<float> kEdgeMargin = {0.0f, 50.0f};

using PlaceEntryFn = void (*)(void* entry);
using ProjectFn    = void (*)(const void* location, std::uint8_t* edge, float* out, float margin);
using ApplyFn      = void (*)(void* widget);

/// One view's placement, for the log: compared without the reticle's point, which moves every frame.
struct Logged
{
    bool         up      = false;  // a wrangle controller exists
    int          entries = 0;      // bubbles placed (entries whose bubble widget resolves)
    bool         reticle = false;
    std::uint8_t edge    = 0;
    float        x       = 0.0f;  // the reticle's point in the last placement (the message only)
    float        y       = 0.0f;
    bool Same(const Logged& o) const noexcept
    {
        return up == o.up && entries == o.entries && reticle == o.reticle && edge == o.edge;
    }
};

std::uintptr_t g_manager    = 0;  // set once at install
PlaceEntryFn   g_placeEntry = nullptr;
ProjectFn      g_project    = nullptr;
ApplyFn        g_apply      = nullptr;
// Game thread.
std::uintptr_t g_faultedCtl = 0;  // the controller whose placement faulted: skipped until another wrangle starts
std::array<Logged, players::kMaxPlayers> g_logged{};
std::array<bool, players::kMaxPlayers>   g_loggedOnce{};

using platform::ReadAt;  // the guarded reads: platform/guarded_read.h

void* Resolve(std::uintptr_t weakField) noexcept
{
    return players::weak_object::ObjectAt(reinterpret_cast<const void*>(weakField));
}

void Fault(std::uintptr_t ctl, const wchar_t* what) noexcept
{
    g_faultedCtl = ctl;
    platform::Log(L"wrangle targets: FAILED: {}; this wrangle's targets keep player 1's placement", what);
}

void LogIfChanged(int seat, const Logged& now) noexcept
{
    const auto s = static_cast<std::size_t>(seat);
    if (!platform::Enabled(platform::Level::Debug) || (g_loggedOnce[s] && now.Same(g_logged[s])))
    {
        return;
    }
    g_logged[s]     = now;
    g_loggedOnce[s] = true;
    try
    {
        if (!now.up)
        {
            platform::LogDebug(L"wrangle targets: no wrangle up ({}'s view)", players::SeatName(seat));
            return;
        }
        platform::LogDebug(L"wrangle targets: {}'s view: {} bubble(s) placed from its camera; the reticle {}",
                           players::SeatName(seat), now.entries,
                           now.reticle ? std::format(L"at ({:.0f}, {:.0f}), edge {}", now.x, now.y, now.edge)
                                       : std::wstring(L"not shown (no target)"));
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(kCaptureManager, sizeof(void*));
    const auto place   = platform::FindFunction(image, kPlaceEntry);
    const auto project = platform::FindFunction(image, kProject);
    const auto apply   = platform::FindFunction(image, facts::kApplyRenderTransform);
    g_manager          = manager ? *manager : 0;
    g_placeEntry       = place ? reinterpret_cast<PlaceEntryFn>(*place) : nullptr;
    g_project          = project ? reinterpret_cast<ProjectFn>(*project) : nullptr;
    g_apply            = apply ? reinterpret_cast<ApplyFn>(*apply) : nullptr;
    const bool ready   = g_manager != 0 && g_placeEntry != nullptr && g_project != nullptr && g_apply != nullptr;
    platform::Log(L"wrangle targets: the chocobo wrangle's bubbles and reticle placed from each view's camera: {}",
                  ready ? L"ready" : L"UNAVAILABLE (a check failed; every view keeps player 1's placement)");
}

void Place(int seat) noexcept
{
    if (g_manager == 0 || g_placeEntry == nullptr || g_project == nullptr || g_apply == nullptr || seat < 0 ||
        seat >= players::kMaxPlayers)
    {
        return;
    }
    const auto manager = ReadAt<std::uintptr_t>(g_manager);
    const auto ctl     = manager != 0 ? ReadAt<std::uintptr_t>(manager + kController) : 0;
    Logged     now{.up = ctl != 0};
    if (ctl == 0 || ctl == g_faultedCtl)
    {
        LogIfChanged(seat, now);
        return;
    }
    // The bubbles: the game's own placer per entry whose bubble widget exists (the game runs it in the active states
    // only; before the widgets are made or after they are released the entry has none), through the primary
    // controller the pass holds.
    const auto entries = ReadAt<std::uintptr_t>(ctl + kEntries);
    const auto count   = ReadAt<std::int32_t>(ctl + kEntryCount);
    for (std::int32_t i = 0; entries != 0 && i < count && static_cast<std::size_t>(i) < kMaxEntries; ++i)
    {
        const auto entry = entries + kEntrySize * static_cast<std::uint32_t>(i);
        if (Resolve(entry + kEntryBubble) == nullptr)
        {
            continue;
        }
        if (!platform::CallGuarded(g_placeEntry, reinterpret_cast<void*>(entry)))
        {
            Fault(ctl, L"the game's bubble placement faulted");
            return;
        }
        ++now.entries;
    }
    // The reticle: the stored target location projected again, and the widget's transform applied. The point starts
    // as the widget's current translation, so a projection the helper declines leaves it where it is.
    void* const reticle = Resolve(ctl + kReticleWeak);
    if (reticle != nullptr && Resolve(ctl + kTargetWeak) != nullptr)
    {
        std::uint8_t edge     = 0;
        float        point[2] = {};
        auto* const  at       = static_cast<std::uint8_t*>(reticle) + kTranslation;
        platform::CopyGuarded(point, at, sizeof(point));
        if (!platform::CallGuarded(g_project, reinterpret_cast<const void*>(ctl + kTargetLocation), &edge,
                                   static_cast<float*>(point), static_cast<float>(kEdgeMargin)) ||
            !platform::WriteGuarded(at, point, sizeof(point)) || !platform::CallGuarded(g_apply, reticle))
        {
            Fault(ctl, L"the reticle's projection faulted");
            return;
        }
        now.reticle = true;
        now.edge    = edge;
        now.x       = point[0];
        now.y       = point[1];
    }
    LogIfChanged(seat, now);
}

}  // namespace lss::hud::wrangle_targets
