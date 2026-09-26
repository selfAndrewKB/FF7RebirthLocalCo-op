#include "hud/popups.h"

#include "hud/popups_code.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "player_two/generation.h"

#include <cstdint>
#include <cstring>
#include <iterator>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::popups {

namespace {

// Facts read from the database (RE-p2-battle-popups.md §1; this build).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kManager     = 0xA0;   // the popup manager (a pointer)
constexpr std::uint32_t  kCanvas      = 0x10;   // TSharedPtr<SEndPopupMessageCanvas> {object, controller}
constexpr std::uint32_t  kSharedCount = 0x8;    // a reference controller's shared count (int32)

// The three lists (TSparseArray: data +0, allocation bits inline +0x10 or heap +0x20, NumBits +0x28).
struct List
{
    std::uint32_t member;   // in the manager
    std::uint32_t element;  // bytes per element
    bool          umg;      // element +0 is a weak UUserWidget (else a TSharedPtr<SWidget>)
};
constexpr List kLists[] = {
    {0x20, 0xD8, false},  // SEndBattleNumber: damage and heal numbers
    {0x70, 0xD8, false},  // SEndBattleText: Dodge, NoEffect...
    {0xC0, 0xD0, true},   // UMG popups: level-up and similar
};
constexpr std::uint32_t kListData       = 0x0;
constexpr std::uint32_t kListInlineBits = 0x10;
constexpr std::uint32_t kListHeapBits   = 0x20;
constexpr std::uint32_t kListNumBits    = 0x28;
constexpr std::int32_t  kMaxBits        = 4096;

// The wrappers: prologues up to the stack probe, checked before their first instruction is replaced.
constexpr std::uintptr_t kPlaceSlate           = 0x140F5F708;
constexpr std::uint8_t   kPlaceSlatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x60,
                                                  0x00, 0x00, 0x00, 0xE8, 0x18, 0xF8, 0x14, 0x01};
constexpr std::uintptr_t kPlaceUmg             = 0x143A41704;
constexpr std::uint8_t   kPlaceUmgPrologue[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                                  0x41, 0x56, 0xB8, 0x70, 0x00, 0x00, 0x00, 0xE8, 0x14, 0xD8, 0x66, 0xFE};
constexpr std::uintptr_t kEntryLength          = sizeof(kPopupEntryBytes);

// FWeakObjectPtr::Get (hud/hud_updates.cpp).
constexpr std::uintptr_t kWeakGet           = 0x141905998;
constexpr std::uint8_t   kWeakGetPrologue[] = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45, 0x85, 0xC9, 0x74, 0x4F};

constexpr int kMaxLines = 40;

using PlaceFn   = std::uintptr_t (*)(void* out, void* controller, void* canvas, void* widget, const float* location,
                                     std::uint64_t offset, std::uint8_t clamp);
using WeakGetFn = void* (*)(const void* weak);

// Installed once, then read. The record is read and written by the code near the game.
platform::CodeRedirect g_slateCode;
platform::CodeRedirect g_umgCode;
PopupRecord            g_record;
bool                   g_installed   = false;
std::uintptr_t         g_endUISystem = 0;
PlaceFn                g_placeSlate  = nullptr;
PlaceFn                g_placeUmg    = nullptr;
WeakGetFn              g_weakGet     = nullptr;

// Game thread.
bool g_failed = false;
int  g_lines  = 0;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
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

bool PlaceGuarded(PlaceFn place, void* controller, void* canvas, void* widget, const PopupPlacement& placement) noexcept
{
    __try
    {
        float         out[2]{};
        std::uint64_t offset = 0;
        std::memcpy(&offset, placement.offset, sizeof(offset));
        place(out, controller, canvas, widget, placement.location, offset, placement.clamp);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The last placement the game made this frame for `key`, or null.
const PopupPlacement* Recorded(std::uint64_t key) noexcept
{
    const std::uint32_t count = g_record.count < kPopupRecordCapacity ? g_record.count : kPopupRecordCapacity;
    for (std::uint32_t i = count; i-- > 0;)
    {
        if (g_record.placements[i].key == key)
        {
            return &g_record.placements[i];
        }
    }
    return nullptr;
}

bool Alive(std::uintptr_t referenceController) noexcept
{
    std::int32_t shared = 0;
    return referenceController != 0 && Read(referenceController + kSharedCount, shared) && shared > 0;
}

/// Places again every popup of `list` recorded this frame. Returns how many; -1 on an exception.
int ReplayList(std::uintptr_t manager, const List& list, void* canvas, void* playerTwo) noexcept
{
    const std::uintptr_t base = manager + list.member;
    std::uintptr_t       data = 0, heapBits = 0;
    std::int32_t         numBits = 0;
    if (!Read(base + kListData, data) || data == 0 || !Read(base + kListHeapBits, heapBits) ||
        !Read(base + kListNumBits, numBits) || numBits <= 0 || numBits > kMaxBits)
    {
        return 0;
    }
    const std::uintptr_t bits   = heapBits != 0 ? heapBits : base + kListInlineBits;
    int                  placed = 0;
    for (std::int32_t i = 0; i < numBits; ++i)
    {
        std::uint32_t word = 0;
        if (!Read(bits + sizeof(std::uint32_t) * static_cast<std::uint32_t>(i >> 5), word) || (word & (1u << (i & 31))) == 0)
        {
            continue;  // a free slot
        }
        const std::uintptr_t element = data + static_cast<std::uintptr_t>(i) * list.element;
        std::uint64_t        key     = 0;
        void*                widget  = nullptr;  // the wrapper's r9: the entry's own TSharedPtr, or the UUserWidget
        if (list.umg)
        {
            widget = WeakGetGuarded(reinterpret_cast<const void*>(element));
            key    = reinterpret_cast<std::uint64_t>(widget);
        }
        else
        {
            std::uintptr_t controller = 0;
            if (!Read(element, key) || !Read(element + 8, controller) || !Alive(controller))
            {
                continue;
            }
            widget = reinterpret_cast<void*>(element);
        }
        const PopupPlacement* recorded = key != 0 ? Recorded(key) : nullptr;
        if (recorded == nullptr)
        {
            continue;  // not placed by the game this frame
        }
        void* controller = playerTwo != nullptr ? playerTwo : reinterpret_cast<void*>(recorded->controller);
        if (!PlaceGuarded(list.umg ? g_placeUmg : g_placeSlate, controller, canvas, widget, *recorded))
        {
            return -1;
        }
        ++placed;
    }
    return placed;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system      = image.Rebase(kEndUISystem, kManager + sizeof(void*));
    const auto slate       = image.Rebase(kPlaceSlate, kEntryLength);
    const auto umg         = image.Rebase(kPlaceUmg, kEntryLength);
    const auto weakGet     = image.Rebase(kWeakGet);
    if (!system || !slate || !umg || !weakGet || !image.BytesMatch(kPlaceSlate, kPlaceSlatePrologue) ||
        !image.BytesMatch(kPlaceUmg, kPlaceUmgPrologue) || !image.BytesMatch(kWeakGet, kWeakGetPrologue))
    {
        platform::Log(L"popups: the placement code is not what the research read; popups stay player 1's in both halves");
        return false;
    }
    g_endUISystem = *system;
    g_placeSlate  = reinterpret_cast<PlaceFn>(*slate);
    g_placeUmg    = reinterpret_cast<PlaceFn>(*umg);
    g_weakGet     = reinterpret_cast<WeakGetFn>(*weakGet);

    // The capture code reads and writes g_record, in this DLL: pin it first.
    const auto slateCode = PopupCaptureCode(g_record, PopupKey::Slate);
    const auto umgCode   = PopupCaptureCode(g_record, PopupKey::Umg);
    if (!platform::PinThisModule() || !g_slateCode.Install(*slate, kPopupEntryBytes, slateCode, *slate + kEntryLength))
    {
        platform::Log(L"popups: the Slate placement could not be followed; popups stay player 1's in both halves");
        return false;
    }
    if (!g_umgCode.Install(*umg, kPopupEntryBytes, umgCode, *umg + kEntryLength))
    {
        g_slateCode.Restore();
        platform::Log(L"popups: the UMG placement could not be followed; popups stay player 1's in both halves");
        return false;
    }
    g_installed = true;
    platform::Log(L"popups: ready (each battle popup placed again in each player's view)");
    return true;
}

void Remove() noexcept
{
    g_installed = false;
    const bool all = (!g_slateCode.IsInstalled() || g_slateCode.Restore()) && (!g_umgCode.IsInstalled() || g_umgCode.Restore());
    if (!all)
    {
        platform::Log(L"popups: a wrapper was changed after us; left in place (it records, and places as the game)");
    }
}

void Replay(bool playerTwo) noexcept
{
    std::uintptr_t manager = 0;
    std::uintptr_t canvas[2]{};  // the manager's TSharedPtr, copied: the wrappers read only its object
    if (!g_installed || g_failed || g_record.count == 0 || !Read(g_endUISystem + kManager, manager) || manager == 0 ||
        !Read(manager + kCanvas, canvas) || canvas[0] == 0 || !Alive(canvas[1]))
    {
        return;
    }
    void* controller = playerTwo ? player_two::generation::PlayerTwoController() : nullptr;
    if (playerTwo && controller == nullptr)
    {
        return;
    }
    int counts[3]{};
    g_record.replaying = 1;
    for (std::size_t i = 0; i < std::size(kLists) && !g_failed; ++i)
    {
        counts[i] = ReplayList(manager, kLists[i], canvas, controller);
        g_failed  = counts[i] < 0;
    }
    g_record.replaying = 0;
    if (g_failed)
    {
        platform::Log(L"popups: a placement raised an exception; popups are no longer placed per view");
    }
    else if (playerTwo && counts[0] + counts[1] + counts[2] > 0 && g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"popups: placed again in player 2's view: {} numbers, {} texts, {} UMG ({} recorded)", counts[0],
                      counts[1], counts[2], g_record.count);
    }
}

void EndFrame() noexcept
{
    g_record.count = 0;
}

}  // namespace lss::hud::popups
