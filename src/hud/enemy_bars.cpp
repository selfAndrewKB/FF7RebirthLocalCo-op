#include "hud/enemy_bars.h"

#include "hud/enemy_bars_code.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"

#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::enemy_bars {

namespace {

// Facts read from the database (RE-p2-enemy-markers.md §7; this build).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;  // g_EndUISystem (data)
constexpr std::uint32_t  kManager     = 0xF8;         // the enemy-status manager (a pointer)
constexpr std::uint32_t  kGate        = 0xC8;         // u8: the manager updates its entries only when set

// The entries (§7.1): a TSet's elements at +0x10, allocation bits inline at +0x20 or on the heap at
// +0x30 when set, NumBits at +0x38; each element 0x180 bytes: +0 source, +8 its reference
// controller (thread-safe: +0xC weak count), +0x10 = p, whose first field is the weak widget.
constexpr std::uint32_t kData        = 0x10;
constexpr std::uint32_t kInlineBits  = 0x20;
constexpr std::uint32_t kHeapBits    = 0x30;
constexpr std::uint32_t kNumBits     = 0x38;
constexpr std::uint32_t kElement     = 0x180;
constexpr std::uint32_t kEntryP      = 0x10;
constexpr std::int32_t  kMaxBits     = 4096;
constexpr std::uint32_t kWeakCount   = 0xC;
// p: the cached screen point, the y offset, and the flags that make the tick's final step run.
constexpr std::uint32_t kScreenPoint = 0xB8;
constexpr std::uint32_t kYOffset     = 0x134;
constexpr std::uint32_t kFinalFlags  = 0x155;  // 3 bytes

// EndUI__MF8_ProjectEntryByWidget: its entry checked, then its second instruction replaced.
constexpr std::uintptr_t kByWidget         = 0x14176201C;
constexpr std::uint8_t   kByWidgetPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56,
                                                0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C};
constexpr std::uintptr_t kByWidgetSite   = 0x141762021;
constexpr std::uintptr_t kByWidgetResume = 0x141762026;

// The tick's one call to EndUI__MF8_ProjectEntry_Targeted (§7.3): rcx mgr, rdx widget, r8 &key
// {source, controller} (the callee releases one weak reference), r9 &loc, then scratch, halfSize,
// skipLeaderClamp on the stack.
constexpr std::uintptr_t kTargetedCall = 0x140DBA664;
constexpr std::uintptr_t kTargeted     = 0x140A6699C;

// UUserWidget::SetPositionInViewport(widget, FVector2D, bRemoveDPIScale) (hud/position_probe.cpp).
constexpr std::uintptr_t kSetPosition           = 0x140CC39E8;
constexpr std::uint8_t   kSetPositionPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};

// FWeakObjectPtr::Get (hud/hud_updates.cpp).
constexpr std::uintptr_t kWeakGet           = 0x141905998;
constexpr std::uint8_t   kWeakGetPrologue[] = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45, 0x85, 0xC9, 0x74, 0x4F};

using ByWidgetFn    = void (*)(void* manager, void* widget, const float* location);
using TargetedFn    = std::uint64_t (*)(void* manager, void* widget, void* key, const float* location, std::int32_t scratch,
                                        float halfSize, std::uint8_t skipLeaderClamp);
using SetPositionFn = void (*)(void* widget, std::uint64_t position, bool removeDpiScale);
using WeakGetFn     = void* (*)(const void* weak);

struct Key  // a thread-safe TWeakPtr, as the targeted function takes it
{
    std::uintptr_t source     = 0;
    std::uintptr_t controller = 0;
};

// Installed once, then read. The record is read and written by the code near the game.
platform::CodeRedirect g_byWidgetCode;
platform::CallRedirect g_targetedCall;
BarRecord              g_record;
bool                   g_installed    = false;
std::uintptr_t         g_endUISystem  = 0;
ByWidgetFn             g_byWidget     = nullptr;
SetPositionFn          g_setPosition  = nullptr;
WeakGetFn              g_weakGet      = nullptr;

constexpr int kMaxLines = 40;

// Game thread.
bool g_failed = false;
int  g_lines  = 0;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

/// Replaces the tick's call to the targeted placement: records it, then places as the game does.
std::uint64_t Targeted(void* manager, void* widget, void* key, const float* location, std::int32_t scratch, float halfSize,
                       std::uint8_t skipLeaderClamp) noexcept
{
    if (g_record.replaying == 0 && g_record.count < kBarRecordCapacity && location != nullptr)
    {
        BarPlacement& placement = g_record.placements[g_record.count++];
        placement.widget          = reinterpret_cast<std::uint64_t>(widget);
        std::memcpy(placement.location, location, sizeof(placement.location));
        placement.halfSize        = halfSize;
        placement.targeted        = 1;
        placement.skipLeaderClamp = skipLeaderClamp;
    }
    return reinterpret_cast<TargetedFn>(g_targetedCall.Original())(manager, widget, key, location, scratch, halfSize,
                                                                   skipLeaderClamp);
}

void* WeakGetGuarded(const void* weak) noexcept
{
    __try
    {
        return g_weakGet(weak);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return nullptr;
    }
}

bool PlaceGuarded(std::uintptr_t manager, void* widget, const BarPlacement& placement, std::uintptr_t entry) noexcept
{
    __try
    {
        if (placement.targeted == 0)
        {
            g_byWidget(reinterpret_cast<void*>(manager), widget, placement.location);
            return true;
        }
        Key key{*reinterpret_cast<const std::uintptr_t*>(entry), *reinterpret_cast<const std::uintptr_t*>(entry + 8)};
        if (key.controller == 0)  // a null key would match entries whose source is gone
        {
            return true;
        }
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(key.controller + kWeakCount));  // the callee releases one
        reinterpret_cast<TargetedFn>(g_targetedCall.Original())(reinterpret_cast<void*>(manager), widget, &key,
                                                                placement.location, 0, placement.halfSize,
                                                                placement.skipLeaderClamp);
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
const BarPlacement* Recorded(const void* widget) noexcept
{
    const std::uint32_t count = g_record.count < kBarRecordCapacity ? g_record.count : kBarRecordCapacity;
    for (std::uint32_t i = count; i-- > 0;)
    {
        if (g_record.placements[i].widget == reinterpret_cast<std::uint64_t>(widget))
        {
            return &g_record.placements[i];
        }
    }
    return nullptr;
}

/// The tick's final step for this bar, if the tick takes it (§7.3): the bar at its cached point, its
/// y offset added.
bool FinalStep(void* widget, std::uintptr_t p, const BarPlacement& placement) noexcept
{
    std::uint8_t flags[3]{};
    float        point[2]{};
    float        offset = 0.0f;
    if (!Read(p + kFinalFlags, flags) || !Read(p + kScreenPoint, point) || !Read(p + kYOffset, offset))
    {
        return true;
    }
    if (placement.targeted == 0 && flags[0] == 0 && flags[1] == 0 && flags[2] == 0)
    {
        return true;
    }
    const float   placedAt[2] = {point[0], point[1] + offset};
    std::uint64_t packed   = 0;
    std::memcpy(&packed, placedAt, sizeof(packed));
    return SetPositionGuarded(widget, packed);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system   = image.Rebase(kEndUISystem, kManager + sizeof(void*));
    const auto byWidget = image.Rebase(kByWidget);
    const auto site     = image.Rebase(kByWidgetSite, sizeof(kBarEntryBytes));
    const auto resume   = image.Rebase(kByWidgetResume);
    const auto call     = image.Rebase(kTargetedCall, 5);
    const auto targeted = image.Rebase(kTargeted);
    const auto position = image.Rebase(kSetPosition);
    const auto weakGet  = image.Rebase(kWeakGet);
    if (!system || !byWidget || !site || !resume || !call || !targeted || !position || !weakGet ||
        !image.BytesMatch(kByWidget, kByWidgetPrologue) || !image.BytesMatch(kSetPosition, kSetPositionPrologue) ||
        !image.BytesMatch(kWeakGet, kWeakGetPrologue))
    {
        platform::Log(L"enemy bars: the placement code is not what the research read; bars stay player 1's in both halves");
        return false;
    }
    g_endUISystem = *system;
    g_byWidget    = reinterpret_cast<ByWidgetFn>(*byWidget);
    g_setPosition = reinterpret_cast<SetPositionFn>(*position);
    g_weakGet     = reinterpret_cast<WeakGetFn>(*weakGet);

    // The capture code reads and writes g_record, in this DLL: pin it first.
    const auto code = BarCaptureCode(g_record);
    if (!platform::PinThisModule() || !g_byWidgetCode.Install(*site, kBarEntryBytes, code, *resume))
    {
        platform::Log(L"enemy bars: the untargeted placement could not be followed; bars stay player 1's in both halves");
        return false;
    }
    if (!g_targetedCall.Install(*call, *targeted, reinterpret_cast<std::uintptr_t>(&Targeted)))
    {
        g_byWidgetCode.Restore();
        platform::Log(L"enemy bars: the targeted placement call is not what the research read; bars stay player 1's");
        return false;
    }
    g_installed = true;
    platform::Log(L"enemy bars: ready (each bar placed again in each player's view)");
    return true;
}

void Remove() noexcept
{
    g_installed = false;
    bool all    = (!g_targetedCall.IsInstalled() || g_targetedCall.Restore()) &&
               (!g_byWidgetCode.IsInstalled() || g_byWidgetCode.Restore());
    if (!all)
    {
        platform::Log(L"enemy bars: a site was changed after us; left in place (it records, and places as the game)");
    }
}

void Replay() noexcept
{
    std::uintptr_t manager = 0;
    std::uint8_t   gate    = 0;
    if (!g_installed || g_failed || g_record.count == 0 || !Read(g_endUISystem + kManager, manager) || manager == 0 ||
        !Read(manager + kGate, gate) || gate == 0)
    {
        return;
    }
    std::uintptr_t data = 0, heapBits = 0;
    std::int32_t   numBits = 0;
    if (!Read(manager + kData, data) || data == 0 || !Read(manager + kHeapBits, heapBits) ||
        !Read(manager + kNumBits, numBits) || numBits <= 0 || numBits > kMaxBits)
    {
        return;
    }
    const std::uintptr_t bits = heapBits != 0 ? heapBits : manager + kInlineBits;
    int                  replayed = 0, targeted = 0;
    float                firstBefore[2]{}, firstAfter[2]{};
    g_record.replaying        = 1;
    for (std::int32_t i = 0; i < numBits && !g_failed; ++i)
    {
        std::uint32_t word = 0;
        if (!Read(bits + sizeof(std::uint32_t) * static_cast<std::uint32_t>(i >> 5), word) || (word & (1u << (i & 31))) == 0)
        {
            continue;  // a free slot
        }
        const std::uintptr_t entry  = data + static_cast<std::uintptr_t>(i) * kElement;
        const std::uintptr_t p      = entry + kEntryP;
        void*                widget = WeakGetGuarded(reinterpret_cast<const void*>(p));
        const BarPlacement*  placed = widget != nullptr ? Recorded(widget) : nullptr;
        if (placed == nullptr)
        {
            continue;  // not placed by the game this frame
        }
        if (replayed == 0)
        {
            Read(p + kScreenPoint, firstBefore);
        }
        if (!PlaceGuarded(manager, widget, *placed, entry) || !FinalStep(widget, p, *placed))
        {
            g_failed = true;
            platform::Log(L"enemy bars: a placement raised an exception; bars are no longer placed per view");
        }
        if (replayed++ == 0)
        {
            Read(p + kScreenPoint, firstAfter);
        }
        targeted += placed->targeted;
    }
    g_record.replaying = 0;
    if (replayed > 0 && g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"enemy bars: {} placed again ({} targeted) of {} recorded; the first bar's point ({:.0f},{:.0f}) -> "
                      L"({:.0f},{:.0f})",
                      replayed, targeted, g_record.count, firstBefore[0], firstBefore[1], firstAfter[0], firstAfter[1]);
    }
}

void EndFrame() noexcept
{
    g_record.count = 0;
}

}  // namespace lss::hud::enemy_bars
