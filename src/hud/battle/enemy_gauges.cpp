#include "hud/battle/enemy_gauges.h"

#include "battle/lock_on.h"
#include "facts/hud.h"
#include "hud/battle/enemy_gauges_code.h"
#include "hud/battle/target_views.h"
#include "hud/hud_rules.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/game_thread_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/settled_log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "scopes/leader_scope.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::enemy_gauges {

namespace {

// Facts read from the database (research/RE-p2-enemy-markers.md, RE-p2-bar-targeted.md; this build).
constexpr std::uint32_t  kGate        = 0xC8;         // u8: the manager updates its entries only when set
constexpr std::uint32_t  kLockOnKey   = 0x140;        // player 1's targeted key, a TWeakPtr {object, controller}

// The entries: a TSet whose element TSparseArray is at +0x10 (platform/sparse_array.h); each
// element 0x180 bytes: +0 source, +8 its reference controller (thread-safe: +0xC weak count), +0x10 = p,
// whose first field is the weak widget.
constexpr std::uint32_t kEntries     = 0x10;
constexpr std::uint32_t kElement     = 0x180;
constexpr std::uint32_t kEntryP      = 0x10;
constexpr std::uint32_t kWeakCount   = 0xC;
// p: the cached screen point, the y offset (the "targeted" ease), and the flags that make the tick's
// final step run.
constexpr std::uint32_t kScreenPoint = 0xB8;
constexpr std::uint32_t kYOffset     = 0x134;
constexpr std::uint32_t kFinalFlags  = 0x155;  // 3 bytes
// The targeted clamp's half size from the source's owner (RE-p2-bar-targeted.md): c = *(owner+0x298),
// B = 3 floats at c+0x1E0, max(min(B.x, B.y) * c[0x4AC], B.z * c[0x4A8]).
constexpr std::uint32_t kOwnerSlot   = 0x28;
constexpr std::uint32_t kHalfSource  = 0x298;
constexpr std::uint32_t kHalfBounds  = 0x1E0;
// c is the owner's UCapsuleComponent (ACharacter::CapsuleComponent +0x298): +0x4AC CapsuleRadius, +0x4A8 CapsuleHalfHeight.
// 1.005: UPrimitiveComponent shrank by 0x10; UpdateEntries reads [rax+49Ch] and [rax+498h].
constexpr platform::PerBuild<std::uint32_t> kHalfScaleXY = {0x4AC, 0x49C};
constexpr platform::PerBuild<std::uint32_t> kHalfScaleZ  = {0x4A8, 0x498};

// EndUI__MF8_ProjectEntryByWidget: its entry checked, then its second instruction replaced.
constexpr std::uint8_t   kByWidgetPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56,
                                                0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C};
constexpr platform::FunctionFact kByWidget{L"EndUI__MF8_ProjectEntryByWidget", 0x14176201C, kByWidgetPrologue};
constexpr std::uintptr_t kByWidgetSite   = 0x141762021;
constexpr std::uintptr_t kByWidgetResume = 0x141762026;

// The tick's one call to EndUI__MF8_ProjectEntry_Targeted: rcx mgr, rdx widget, r8 &key
// {source, controller} (the callee releases one weak reference), r9 &loc, then scratch, halfSize,
// skipLeaderClamp on the stack.
constexpr std::uintptr_t kTargetedCall = 0x140DBA664;
constexpr std::uint8_t   kTargetedPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                                0x41, 0x56, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x7C, 0x85, 0x64, 0x01};
constexpr platform::FunctionFact kTargeted{L"EndUI__MF8_ProjectEntry_Targeted", 0x140A6699C, kTargetedPrologue};

// The tick's one call to EndUI__MF8_IsEntryForcedTargeted_DecTimer15C(p) (bar-targeted R1), made for
// every entry it places: case (d) of the targeted rule. Never called by the mod (it decrements a timer).
constexpr std::uintptr_t kForcedCall = 0x140DBA287;
constexpr std::uint8_t   kForcedPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                              0x0E, 0x86, 0x23, 0x01};
constexpr platform::FunctionFact kForced{L"EndUI__MF8_IsEntryForcedTargeted_DecTimer15C", 0x140E76904, kForcedPrologue};

// EndUI__MF8_ApplyYOffsetLerp_SetPosition(unused, widget, p): one step of the ease on World dt, then
// the gauge at its cached point plus the offset.
constexpr std::uint8_t   kYStepPrologue[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x86, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24,
                                             0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x0B, 0x3E, 0x52, 0x01};
constexpr platform::FunctionFact kYStep{L"EndUI__MF8_ApplyYOffsetLerp_SetPosition", 0x140B8B10C, kYStepPrologue};

// EndUI__MF8_LeaderDistanceScaleLerp(unused, widget, owner, float* state, unused, caseC): eases a
// gauge's size (p+0x38) toward one set by its enemy's distance from the leader (read through
// ResolvePartyLeaderWeakPtr: a LeaderScope answers it), then writes the widget's render scale
// (+0x98/+0x9C) and applies it (RE-p2-bar-targeted.md).
constexpr std::uint8_t   kScaleStepPrologue[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x98, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x08,
                                                 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00, 0xE8, 0xB6, 0x3F,
                                                 0x52, 0x01};
constexpr platform::FunctionFact kScaleStep{L"EndUI__MF8_LeaderDistanceScaleLerp", 0x140B8AF5C, kScaleStepPrologue};
// The render-transform apply after the scale write: facts::kApplyRenderTransform (shared with the wrangle targets).
constexpr std::uint32_t  kScale       = 0x38;   // p: the gauge's size state
constexpr std::uint32_t  kRenderScale = 0x98;   // widget: render scale x, y

// FWeakObjectPtr::Get (hud/hud_updates.cpp).

using ByWidgetFn    = void (*)(void* manager, void* widget, const float* location);
using TargetedFn    = std::uint64_t (*)(void* manager, void* widget, void* key, const float* location, std::int32_t scratch,
                                        float halfSize, std::uint8_t skipLeaderClamp);
using ForcedFn      = std::uint8_t (*)(void* p);
using YStepFn       = void (*)(void* unused, void* widget, void* p);
using SetPositionFn = void (*)(void* widget, std::uint64_t position, bool removeDpiScale);
using OwnerFn       = void* (*)(void* source);
using ScaleStepFn   = void (*)(void* unused, void* widget, void* owner, float* state, std::int32_t unusedToo, std::uint8_t caseC);
using ApplyFn       = void (*)(void* widget);

struct Key  // a thread-safe TWeakPtr, as the targeted function takes it
{
    std::uintptr_t source     = 0;
    std::uintptr_t controller = 0;
};

/// An entry's case (d) answer and y-step flags as the tick tested them.
struct Capture
{
    std::uintptr_t p      = 0;
    std::uint8_t   forced = 0;
    std::uint8_t   flags[3]{};
};

/// A seat's copies of one gauge's y offset and size, keyed by the entry's source.
struct SeatOffset
{
    Key   key;
    float y     = 0.0f;
    float scale = 0.0f;
    bool  seen  = false;
};

/// Player 1's size of a gauge, put back after a seat's render.
struct ScaleRestore
{
    void*          widget = nullptr;
    std::uintptr_t p      = 0;
    float          scale  = 0.0f;
    float          render[2]{};
};

// Installed once, then read. The record is read and written by the code near the game.
platform::CodeRedirect g_byWidgetCode;
platform::CallRedirect g_targetedCall;
platform::CallRedirect g_forcedCall;
GaugeRecord            g_record;
bool                   g_installed    = false;
bool                   g_ownTargets   = false;  // a seat's pass classifies by the seat's target
std::uintptr_t         g_endUISystem  = 0;
ByWidgetFn             g_byWidget     = nullptr;
YStepFn                g_yStep        = nullptr;
ScaleStepFn            g_scaleStep    = nullptr;
ApplyFn                g_apply        = nullptr;
SetPositionFn          g_setPosition  = nullptr;


// Game thread.
std::array<Capture, kGaugeRecordCapacity> g_captures{};
std::uint32_t                           g_captureCount = 0;
std::array<std::vector<SeatOffset>, players::kMaxPlayers> g_offsets;  // by seat ([0] unused)
std::vector<ScaleRestore>               g_scaleRestores;  // a seat's pass: sizes to put back after its render
bool         g_failed = false;
/// A seat's target's gauge (or its main target's) as the log shows it, in tenths.
struct TargetGauge
{
    bool         own = false;
    std::int32_t half = 0, before = 0, after = 0, theirs = 0;
    bool         kept = false;
    std::int32_t scale = 0, theirScale = 0;  // in hundredths
    bool operator==(const TargetGauge&) const = default;
};
using TargetGauges = std::array<TargetGauge, 2>;

struct TargetSteady
{
    std::int32_t count = -1;
    TargetGauges gauges{};
    bool operator==(const TargetSteady&) const = default;
};
constexpr std::uint32_t kTargetStillFrames = 30;
using TargetLog = platform::SettledLog<TargetSteady, TargetGauges>;
static_assert(players::kMaxPlayers == 3, "one target log and one logged pass per player, listed below");
std::array<TargetLog, players::kMaxPlayers> g_targetLogs{TargetLog(kTargetStillFrames), TargetLog(kTargetStillFrames),
                                                         TargetLog(kTargetStillFrames)};  // by seat ([0] unused)
std::int32_t g_loggedMismatches = -1;

std::int32_t Tenths(float value) noexcept
{
    return static_cast<std::int32_t>(value * 10.0f + (value < 0.0f ? -0.5f : 0.5f));
}

using platform::Read;  // the guarded reads: platform/guarded.h
using platform::ReadAt;

/// One line of a seat's target gauges at `level` (Debug or Trace). Callers
/// check platform::Enabled(level) before building `text`.
void Note(platform::Level level, const std::wstring& text)
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"enemy gauges: {}", text);
    }
}

/// Replaces the tick's call to the targeted placement: records it, then places as the game does.
std::uint64_t Targeted(void* manager, void* widget, void* key, const float* location, std::int32_t scratch, float halfSize,
                       std::uint8_t skipLeaderClamp) noexcept
{
    if (g_record.replaying == 0 && g_record.count < kGaugeRecordCapacity && location != nullptr)
    {
        GaugePlacement& placement = g_record.placements[g_record.count++];
        placement.widget          = reinterpret_cast<std::uint64_t>(widget);
        std::memcpy(placement.location, location, sizeof(placement.location));
        placement.halfSize        = halfSize;
        placement.targeted        = 1;
        placement.skipLeaderClamp = skipLeaderClamp;
    }
    return reinterpret_cast<TargetedFn>(g_targetedCall.Original())(manager, widget, key, location, scratch, halfSize,
                                                                   skipLeaderClamp);
}

/// Replaces the tick's call to the forced-targeted test: its answer and the entry's flags recorded.
std::uint8_t Forced(void* p) noexcept
{
    const std::uint8_t forced = reinterpret_cast<ForcedFn>(g_forcedCall.Original())(p);
    if (g_record.replaying == 0 && g_captureCount < g_captures.size() && p != nullptr)
    {
        Capture& capture = g_captures[g_captureCount++];
        capture.p        = reinterpret_cast<std::uintptr_t>(p);
        capture.forced   = forced;
        std::memcpy(capture.flags, static_cast<const std::byte*>(p) + kFinalFlags, sizeof(capture.flags));
    }
    return forced;
}

/// One gauge placed by the untargeted or targeted function; the targeted key {source, controller} is
/// the entry's own, with the weak reference the callee releases added first.
bool PlaceGuarded(std::uintptr_t manager, void* widget, bool targeted, const float* location, float halfSize,
                  std::uint8_t skipLeaderClamp, std::uintptr_t entry) noexcept
{
    __try
    {
        if (!targeted)
        {
            g_byWidget(reinterpret_cast<void*>(manager), widget, location);
            return true;
        }
        Key key{*reinterpret_cast<const std::uintptr_t*>(entry), *reinterpret_cast<const std::uintptr_t*>(entry + 8)};
        if (key.controller == 0)  // a null key would match entries whose source is gone
        {
            return true;
        }
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(key.controller + kWeakCount));
        reinterpret_cast<TargetedFn>(g_targetedCall.Original())(reinterpret_cast<void*>(manager), widget, &key, location, 0,
                                                                halfSize, skipLeaderClamp);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The game's y step `steps` times on a seat's offset: swapped into p+0x134 and back, player 1's
/// value always put back.
bool StepGuarded(void* widget, std::uintptr_t p, float& offset, int steps) noexcept
{
    auto* const field = reinterpret_cast<float*>(p + kYOffset);
    float       own   = 0.0f;
    __try
    {
        own = *field;
        __try
        {
            *field = offset;
            for (int i = 0; i < steps; ++i)
            {
                g_yStep(nullptr, widget, reinterpret_cast<void*>(p));
            }
            offset = *field;
        }
        __finally
        {
            *field = own;
        }
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// A seat's size step on one gauge: player 1's size and render scale saved in `saved` first, then
/// the seat's size put in and eased by the game's own step. The seat's stays in place for the rest of
/// its pass (the red ability label reads it); RestoreScaleGuarded puts player 1's back.
bool ScaleGuarded(void* widget, void* owner, std::uintptr_t p, float& scale, std::uint8_t caseC, ScaleRestore& saved) noexcept
{
    __try
    {
        auto* const state  = reinterpret_cast<float*>(p + kScale);
        auto* const render = reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(widget) + kRenderScale);
        saved              = {widget, p, *state, {render[0], render[1]}};
        *state             = scale;
        g_scaleStep(nullptr, widget, owner, state, 0, caseC);
        scale = *state;
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool RestoreScaleGuarded(const ScaleRestore& saved) noexcept
{
    __try
    {
        *reinterpret_cast<float*>(saved.p + kScale) = saved.scale;
        auto* const render = reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(saved.widget) + kRenderScale);
        render[0]          = saved.render[0];
        render[1]          = saved.render[1];
        g_apply(saved.widget);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool SetPositionGuarded(void* widget, std::uint64_t position) noexcept
{
    __try
    {
        g_setPosition(widget, position, false);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The last placement the game made this frame for `widget`, or null.
const GaugePlacement* Recorded(const void* widget) noexcept
{
    return g_record.Last(reinterpret_cast<std::uint64_t>(widget),
                         [](const GaugePlacement& placement) { return placement.widget; });
}

/// The tick's test of this entry this frame, or its flags as they are now if it was not recorded.
Capture Captured(std::uintptr_t p) noexcept
{
    for (std::uint32_t i = g_captureCount; i-- > 0;)
    {
        if (g_captures[i].p == p)
        {
            return g_captures[i];
        }
    }
    Capture now{p};
    Read(p + kFinalFlags, now.flags);
    return now;
}

bool AnyFlag(const Capture& capture) noexcept
{
    return capture.flags[0] != 0 || capture.flags[1] != 0 || capture.flags[2] != 0;
}

/// Player 1's view: the tick's final step for this gauge, if the tick takes it: the gauge at its
/// cached point, its y offset added.
bool FinalStep(void* widget, std::uintptr_t p, const GaugePlacement& placement, const Capture& capture) noexcept
{
    float point[2]{};
    float offset = 0.0f;
    if ((placement.targeted == 0 && !AnyFlag(capture)) || !Read(p + kScreenPoint, point) || !Read(p + kYOffset, offset))
    {
        return true;
    }
    const float   placedAt[2] = {point[0], point[1] + offset};
    std::uint64_t packed   = 0;
    std::memcpy(&packed, placedAt, sizeof(packed));
    return SetPositionGuarded(widget, packed);
}

/// The targeted clamp's half size for a source the tick placed untargeted (RE-p2-bar-targeted.md).
float HalfSize(std::uintptr_t source) noexcept
{
    void*      owner = nullptr;
    const auto slot  = reinterpret_cast<OwnerFn>(ReadAt<std::uintptr_t>(ReadAt<std::uintptr_t>(source) + kOwnerSlot));
    if (!platform::CallGuarded(slot, owner, reinterpret_cast<void*>(source)) || owner == nullptr)
    {
        return 0.0f;
    }
    const auto  c = ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(owner) + kHalfSource);
    float       bounds[3]{};
    if (c == 0 || !Read(c + kHalfBounds, bounds))
    {
        return 0.0f;
    }
    return std::max(std::min(bounds[0], bounds[1]) * ReadAt<float>(c + kHalfScaleXY),
                    bounds[2] * ReadAt<float>(c + kHalfScaleZ));
}

SeatOffset& OffsetFor(std::vector<SeatOffset>& offsets, const Key& key, std::uintptr_t p, bool seedFromGame)
{
    for (SeatOffset& offset : offsets)
    {
        if (offset.key.source == key.source && offset.key.controller == key.controller)
        {
            offset.seen = true;
            return offset;
        }
    }
    // A key new to the walk is a new entry, which the game starts at 0; the first pass after the seat
    // arrives starts from the game's values instead. Its size starts from the game's.
    offsets.push_back({key, seedFromGame ? ReadAt<float>(p + kYOffset) : 0.0f, ReadAt<float>(p + kScale), true});
    return offsets.back();
}

/// Player 1's view: every recorded gauge placed again with the tick's arguments. Also checks the mod's
/// targeted rule against the tick's choice (logged when it disagrees).
bool ReplayPlayerOne(std::uintptr_t manager, int& replayed, int& targeted)
{
    const Key  lockOn{ReadAt<std::uintptr_t>(manager + kLockOnKey), ReadAt<std::uintptr_t>(manager + kLockOnKey + 8)};
    const auto one        = target_views::TargetAndMain(lockOn.source, lockOn.controller);
    int        mismatches = 0;
    bool       ok         = true;
    platform::ForEachSparseElement(manager + kEntries, kElement, [&](std::uintptr_t entry) {
        const std::uintptr_t p      = entry + kEntryP;
        void*                widget = players::weak_object::ObjectAt(reinterpret_cast<const void*>(p));
        const GaugePlacement*  placed = widget != nullptr ? Recorded(widget) : nullptr;
        if (placed == nullptr)
        {
            return true;  // not placed by the game this frame
        }
        const Capture capture = Captured(p);
        if (!PlaceGuarded(manager, widget, placed->targeted != 0, placed->location, placed->halfSize, placed->skipLeaderClamp,
                          entry) ||
            !FinalStep(widget, p, *placed, capture))
        {
            ok = false;
            return false;
        }
        const auto source = ReadAt<std::uintptr_t>(entry);
        const bool rule   = hud_rules::PlayerOneTargetRule(source, one[0], one[1], placed->targeted != 0,
                                                           placed->skipLeaderClamp != 0, capture.forced != 0);
        mismatches += rule != (placed->targeted != 0) ? 1 : 0;
        ++replayed;
        targeted += placed->targeted;
        return true;
    });
    if (mismatches != g_loggedMismatches)
    {
        g_loggedMismatches = mismatches;
        Note(platform::Level::Info, std::format(L"player 1's view: the targeted rule disagrees with the tick on {} gauge(s)", mismatches));
    }
    return ok;
}

/// A seat's view: each recorded gauge classified by the seat's target and placed accordingly, with
/// the seat's own y offset.
bool ReplaySeat(int seat, std::uintptr_t manager, int& replayed, int& targeted)
{
    std::uintptr_t object = 0, controller = 0;
    battle::lock_on::Displayed(seat, object, controller);
    const auto          ownTarget  = target_views::TargetAndMain(object, controller);
    const std::uint64_t memberWeak = scopes::leader_scope::WeakOf(players::generation::MemberOf(seat));
    auto&               offsets    = g_offsets[static_cast<std::size_t>(seat)];
    const bool          seed       = offsets.empty();
    for (SeatOffset& offset : offsets)
    {
        offset.seen = false;
    }
    TargetGauges targets{};
    std::int32_t targetCount = 0;
    bool         ok          = true;
    platform::ForEachSparseElement(manager + kEntries, kElement, [&](std::uintptr_t entry) {
        const std::uintptr_t p      = entry + kEntryP;
        void*                widget = players::weak_object::ObjectAt(reinterpret_cast<const void*>(p));
        const GaugePlacement*  placed = widget != nullptr ? Recorded(widget) : nullptr;
        if (placed == nullptr)
        {
            return true;
        }
        const Capture capture = Captured(p);
        const Key     key{ReadAt<std::uintptr_t>(entry), ReadAt<std::uintptr_t>(entry + 8)};
        const auto    gauge   = hud_rules::ClassifyGauge(key.source, ownTarget[0], ownTarget[1], placed->targeted != 0,
                                                         placed->skipLeaderClamp != 0, capture.forced != 0);
        const bool    own     = gauge.own;
        const bool    main    = gauge.main;
        const bool    caseC   = gauge.caseC;
        const bool    target  = gauge.target;
        const float   half    = !target ? 0.0f : placed->targeted != 0 ? placed->halfSize : HalfSize(key.source);
        {
            const scopes::leader_scope::LeaderScope leader(gauge.leaderScope ? memberWeak : 0);
            if (!PlaceGuarded(manager, widget, target, placed->location, half, caseC ? 1 : 0, entry))
            {
                ok = false;
                return false;
            }
        }
        SeatOffset&      offset = OffsetFor(offsets, key, p, seed);
        const float      before = offset.y;
        const float      theirs = ReadAt<float>(p + kYOffset);
        if ((target || AnyFlag(capture)) && !StepGuarded(widget, p, offset.y, gauge.steps))
        {
            ok = false;
            return false;
        }
        const float theirScale = ReadAt<float>(p + kScale);
        if (g_scaleStep != nullptr)
        {
            // The size from the enemy's distance to the seat's member, the seat's own ease.
            void*      owner = nullptr;
            const auto slot  =
                reinterpret_cast<OwnerFn>(ReadAt<std::uintptr_t>(ReadAt<std::uintptr_t>(key.source) + kOwnerSlot));
            ScaleRestore saved;
            bool         stepped = false;
            if (platform::CallGuarded(slot, owner, reinterpret_cast<void*>(key.source)) && owner != nullptr)
            {
                const scopes::leader_scope::LeaderScope leader(memberWeak);
                stepped = ScaleGuarded(widget, owner, p, offset.scale, caseC ? 1 : 0, saved);
            }
            if (saved.widget != nullptr)
            {
                g_scaleRestores.push_back(saved);  // even after a fault: player 1's values go back
            }
            if (owner != nullptr && !stepped)
            {
                ok = false;
                return false;
            }
        }
        if ((own || main) && targetCount < static_cast<std::int32_t>(targets.size()))
        {
            targets[targetCount++] = {own, Tenths(half), Tenths(before), Tenths(offset.y), Tenths(theirs),
                                      ReadAt<float>(p + kYOffset) == theirs, Tenths(offset.scale * 10.0f),
                                      Tenths(theirScale * 10.0f)};
        }
        ++replayed;
        targeted += target ? 1 : 0;
        return true;
    });
    if (ok)  // a walk stopped early did not see every live gauge
    {
        std::erase_if(offsets, [](const SeatOffset& offset) { return !offset.seen; });
    }
    // Which gauges, whose, and whether player 1's offset was kept log at once; the heights and sizes ease
    // every frame, so they log once they hold.
    TargetSteady steady{targetCount, targets};
    for (TargetGauge& gauge : steady.gauges)
    {
        gauge.before = gauge.after = gauge.theirs = gauge.scale = gauge.theirScale = 0;
    }
    TargetLog& log = g_targetLogs[static_cast<std::size_t>(seat)];
    if (!platform::Enabled(platform::Level::Debug))
    {
        log.Reset();  // off: the next line starts fresh
    }
    else if (log.Offer(steady, targets))
    {
        std::wstring detail;
        for (std::int32_t i = 0; i < targetCount; ++i)
        {
            const TargetGauge& gauge = targets[static_cast<std::size_t>(i)];
            detail += std::format(L" [{} targeted by {} half {} y {:.1f}->{:.1f} (player 1's {:.1f}{}) size {:.2f} (player "
                                  L"1's {:.2f})]",
                                  gauge.own ? L"the player's target" : L"its main target",
                                  gauge.own ? players::SeatName(seat) : L"its part",
                                  gauge.half / 10, gauge.before / 10.0, gauge.after / 10.0, gauge.theirs / 10.0,
                                  gauge.kept ? L", kept" : L", CHANGED", gauge.scale / 100.0, gauge.theirScale / 100.0);
        }
        Note(platform::Level::Debug, std::format(L"{}'s view:{}", players::SeatName(seat),
                                                 detail.empty() ? L" no gauge of their target" : detail));
    }
    return ok;
}

/// 1.005: what Install found for the seats' own targeted look, until the game thread places the forced-targeted call.
struct PendingOwnTargets
{
    std::uintptr_t site = 0, forced = 0, yStep = 0, scaleStep = 0, apply = 0;  // the size step's two 0 if not checked out
};
PendingOwnTargets g_pending;

/// 1.005, on the game thread between its ticks: the forced-targeted call placed (it crosses a 16-byte block), then
/// the seats' own targeted look and sizes turned on as at install on the research build.
void PlaceOwnTargets() noexcept
{
    const PendingOwnTargets pending = g_pending;
    g_pending                       = {};
    if (!g_installed || pending.site == 0)
    {
        return;  // removed meanwhile
    }
    if (g_forcedCall.InstallFromOwnThread(pending.site, pending.forced, reinterpret_cast<std::uintptr_t>(&Forced)))
    {
        g_yStep      = reinterpret_cast<YStepFn>(pending.yStep);
        g_ownTargets = true;
    }
    if (g_ownTargets && pending.scaleStep != 0 && pending.apply != 0)
    {
        g_scaleStep = reinterpret_cast<ScaleStepFn>(pending.scaleStep);
        g_apply     = reinterpret_cast<ApplyFn>(pending.apply);
    }
    platform::Log(L"enemy gauges: player 2's and 3's views {}, sizes {}",
                  g_ownTargets ? L"mark their own targets" : L"mark player 1's target (a check failed)",
                  g_scaleStep != nullptr ? L"from their own distance" : L"from player 1's distance (a check failed)");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system   = players::game_globals::Address(players::game_globals::Global::UiSystem);
    const auto byWidget = platform::FindFunction(image, kByWidget);
    const auto site     = image.Rebase(kByWidgetSite, sizeof(kGaugeEntryBytes));
    const auto resume   = image.Rebase(kByWidgetResume);
    const auto call     = image.Rebase(kTargetedCall, 5);
    const auto targeted = platform::FindFunction(image, kTargeted);
    const auto position = platform::FindFunction(image, facts::kSetPositionInViewport);
    if (!system || !byWidget || !site || !resume || !call || !targeted || !position ||
        !players::weak_object::Known())
    {
        platform::Log(L"enemy gauges: the placement code is not what the research read; gauges stay player 1's in every view");
        return false;
    }
    g_endUISystem = system;
    g_byWidget    = reinterpret_cast<ByWidgetFn>(*byWidget);
    g_setPosition = reinterpret_cast<SetPositionFn>(*position);

    // The capture code reads and writes g_record, in this DLL: pin it first.
    const auto code = GaugeCaptureCode(g_record);
    if (!platform::PinThisModule() || !g_byWidgetCode.Install(*site, kGaugeEntryBytes, code, *resume))
    {
        platform::Log(L"enemy gauges: the untargeted placement could not be followed; gauges stay player 1's in every view");
        return false;
    }
    if (!g_targetedCall.Install(*call, *targeted, reinterpret_cast<std::uintptr_t>(&Targeted)))
    {
        g_byWidgetCode.Restore();
        platform::Log(L"enemy gauges: the targeted placement call is not what the research read; gauges stay player 1's");
        return false;
    }
    g_installed = true;

    // The seats' own targeted look: the forced-targeted capture, the y step, the leader scope.
    const auto forcedSite = image.Rebase(kForcedCall, 5);
    const auto forced     = platform::FindFunction(image, kForced);
    const auto yStep      = platform::FindFunction(image, kYStep);
    const bool ownChecks  = forcedSite && forced && yStep && scopes::leader_scope::Installed();
    const auto scaleStep  = platform::FindFunction(image, kScaleStep);
    const auto apply      = platform::FindFunction(image, facts::kApplyRenderTransform);
    const bool sizes      = scaleStep && apply;
    if (ownChecks && platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        // The forced-targeted call crosses a 16-byte block in 1.005: it is placed from the game thread, between its
        // ticks (platform/game_thread_sites.h); the size step's checks are made now.
        g_pending = {*forcedSite, *forced, *yStep, sizes ? *scaleStep : 0, sizes ? *apply : 0};
        if (platform::game_thread_sites::Queue(&PlaceOwnTargets))
        {
            platform::Log(L"enemy gauges: ready (each gauge placed again in each player's view; players 2 and 3's own targets follow "
                          L"once the game thread places their call (1.005: it crosses a 16-byte block))");
            return true;
        }
        g_pending = {};
    }
    if (ownChecks && g_forcedCall.Install(*forcedSite, *forced, reinterpret_cast<std::uintptr_t>(&Forced)))
    {
        g_yStep      = reinterpret_cast<YStepFn>(*yStep);
        g_ownTargets = true;
    }
    if (g_ownTargets && sizes)
    {
        g_scaleStep = reinterpret_cast<ScaleStepFn>(*scaleStep);
        g_apply     = reinterpret_cast<ApplyFn>(*apply);
    }
    platform::Log(L"enemy gauges: ready (each gauge placed again in each player's view; player 2's and 3's views {}, sizes {})",
                  g_ownTargets ? L"mark their own targets" : L"mark player 1's target (a check failed)",
                  g_scaleStep != nullptr ? L"from their own distance" : L"from player 1's distance (a check failed)");
    return true;
}

void Remove() noexcept
{
    g_pending    = {};  // a placement still queued does nothing
    g_installed  = false;
    g_ownTargets = false;
    bool all     = (!g_forcedCall.IsInstalled() || g_forcedCall.Restore()) &&
               (!g_targetedCall.IsInstalled() || g_targetedCall.Restore()) &&
               (!g_byWidgetCode.IsInstalled() || g_byWidgetCode.Restore());
    if (!all)
    {
        platform::Log(L"enemy gauges: a site was changed after us; left in place (it records, and places as the game)");
    }
}

namespace {

/// A seat's replay inputs and result, as the log shows them (on change).
struct PassState
{
    std::uint8_t gate = 0;
    std::int32_t recorded = 0, replayed = 0, targeted = 0;
    bool         list = false;
    bool operator==(const PassState&) const = default;
};
std::array<PassState, players::kMaxPlayers>     g_loggedPass{PassState{0xFF}, PassState{0xFF}, PassState{0xFF}};
std::array<std::uint32_t, players::kMaxPlayers> g_listOpenings{};  // each seat's target list openings seen: each
                                                                   // logs the pass afresh

void LogSeatPass(int seat, const PassState& now) noexcept
{
    const auto s = static_cast<std::size_t>(seat);
    if (const auto openings = battle::lock_on::TargetListOpenings(seat); openings != g_listOpenings[s])
    {
        g_listOpenings[s] = openings;
        g_loggedPass[s]   = PassState{0xFF};
    }
    if (!platform::Enabled(platform::Level::Trace) || now == g_loggedPass[s])
    {
        return;
    }
    g_loggedPass[s] = now;
    try
    {
        Note(platform::Level::Trace, std::format(L"{}'s pass: gate {} recorded {} placed {} ({} targeted){}",
                                                 players::SeatName(seat), now.gate, now.recorded, now.replayed,
                                                 now.targeted, now.list ? L" | their target list open" : L""));
    }
    catch (...)
    {
    }
}

}  // namespace

void Replay(int seat) noexcept
{
    std::uintptr_t manager = 0;
    std::uint8_t   gate    = 0;
    const bool     read    = Read(g_endUISystem + facts::kUIEnemyStatus, manager) && manager != 0 &&
                             Read(manager + kGate, gate);
    const bool     isSeat  = players::SeatIndex(seat);
    if (isSeat && g_installed && !g_failed && (!read || gate == 0 || g_record.count == 0))
    {
        LogSeatPass(seat, {gate, static_cast<std::int32_t>(g_record.count), 0, 0, battle::lock_on::TargetListOpen(seat)});
    }
    if (!g_installed || g_failed || g_record.count == 0 || !read || gate == 0)
    {
        return;
    }
    int  replayed = 0, targeted = 0;
    bool ok       = true;
    {
        const Replaying<GaugeRecord> replaying(g_record);  // the mod's own placements are not recorded
        try
        {
            ok = isSeat && g_ownTargets ? ReplaySeat(seat, manager, replayed, targeted)
                                        : ReplayPlayerOne(manager, replayed, targeted);
        }
        catch (...)
        {
        }
    }
    if (!ok)
    {
        g_failed = true;
        platform::Log(L"enemy gauges: a placement raised an exception; gauges are no longer placed per view");
        return;
    }
    if (isSeat)
    {
        LogSeatPass(seat, {gate, static_cast<std::int32_t>(g_record.count), replayed, targeted,
                           battle::lock_on::TargetListOpen(seat)});
    }
    if (replayed > 0 && platform::Enabled(platform::Level::Trace))  // every HUD pass: Trace (platform/log.h)
    {
        platform::LogTrace(L"enemy gauges: {} placed again for {}'s view ({} targeted) of {} recorded", replayed,
                           players::SeatName(isSeat ? seat : 0), targeted, g_record.count);
    }
}

void EndSeatPass() noexcept
{
    for (const ScaleRestore& saved : g_scaleRestores)
    {
        if (!RestoreScaleGuarded(saved))
        {
            platform::Log(L"enemy gauges: putting a gauge's size back raised an exception");
        }
    }
    g_scaleRestores.clear();
}

void SeatOut(int seat) noexcept
{
    if (players::SeatIndex(seat))
    {
        g_offsets[static_cast<std::size_t>(seat)].clear();  // its next pass starts from the game's values
    }
}

void EndFrame() noexcept
{
    EndSeatPass();  // in case a seat's pass did not reach its own end
    g_record.EndFrame();
    g_captureCount = 0;
}

}  // namespace lss::hud::enemy_gauges
