#include "hud/battle/popups.h"

#include "facts/hud.h"
#include "hud/battle/popups_code.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/weak_object.h"

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
constexpr std::uint32_t  kManager     = 0xA0;   // the popup manager (a pointer)
constexpr std::uint32_t  kCanvas      = 0x10;   // TSharedPtr<SEndPopupMessageCanvas> {object, controller}
constexpr std::uint32_t  kSharedCount = 0x8;    // a reference controller's shared count (int32)

// The three lists (TSparseArrays, platform/sparse_array.h).
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

// The wrappers: prologues up to the stack probe, checked before their first instruction is replaced.
constexpr std::uint8_t   kPlaceSlatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x60,
                                                  0x00, 0x00, 0x00, 0xE8, 0x18, 0xF8, 0x14, 0x01};
constexpr platform::FunctionFact kPlaceSlate{L"the Slate popup's place wrapper", 0x140F5F708, kPlaceSlatePrologue};
constexpr std::uint8_t   kPlaceUmgPrologue[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                                  0x41, 0x56, 0xB8, 0x70, 0x00, 0x00, 0x00, 0xE8, 0x14, 0xD8, 0x66, 0xFE};
constexpr platform::FunctionFact kPlaceUmg{L"the UMG popup's place wrapper", 0x143A41704, kPlaceUmgPrologue};
constexpr std::uintptr_t kEntryLength          = sizeof(kPopupEntryBytes);

// FWeakObjectPtr::Get (hud/hud_updates.cpp).


using PlaceFn   = std::uintptr_t (*)(void* out, void* controller, void* canvas, void* widget, const float* location,
                                     std::uint64_t offset, std::uint8_t clamp);

// Installed once, then read. The record is read and written by the code near the game.
platform::CodeRedirect g_slateCode;
platform::CodeRedirect g_umgCode;
PopupRecord            g_record;
bool                   g_installed   = false;
std::uintptr_t         g_endUISystem = 0;
PlaceFn                g_placeSlate  = nullptr;
PlaceFn                g_placeUmg    = nullptr;

// Game thread.
bool g_failed = false;

using platform::Read;  // the guarded reads: platform/guarded.h

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
    return g_record.Last(key, [](const PopupPlacement& placement) { return placement.key; });
}

bool Alive(std::uintptr_t referenceController) noexcept
{
    std::int32_t shared = 0;
    return referenceController != 0 && Read(referenceController + kSharedCount, shared) && shared > 0;
}

/// Places again every popup of `list` recorded this frame. Returns how many; -1 on an exception.
int ReplayList(std::uintptr_t manager, const List& list, void* canvas, void* playerTwo) noexcept
{
    int placed = 0;
    platform::ForEachSparseElement(manager + list.member, list.element, [&](std::uintptr_t element) {
        std::uint64_t key    = 0;
        void*         widget = nullptr;  // the wrapper's r9: the entry's own TSharedPtr, or the UUserWidget
        if (list.umg)
        {
            widget = players::weak_object::ObjectAt(reinterpret_cast<const void*>(element));
            key    = reinterpret_cast<std::uint64_t>(widget);
        }
        else
        {
            std::uintptr_t controller = 0;
            if (!Read(element, key) || !Read(element + 8, controller) || !Alive(controller))
            {
                return true;
            }
            widget = reinterpret_cast<void*>(element);
        }
        const PopupPlacement* recorded = key != 0 ? Recorded(key) : nullptr;
        if (recorded == nullptr)
        {
            return true;  // not placed by the game this frame
        }
        void* controller = playerTwo != nullptr ? playerTwo : reinterpret_cast<void*>(recorded->controller);
        if (!PlaceGuarded(list.umg ? g_placeUmg : g_placeSlate, controller, canvas, widget, *recorded))
        {
            placed = -1;
            return false;
        }
        ++placed;
        return true;
    });
    return placed;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system      = image.Rebase(facts::kEndUISystem, kManager + sizeof(void*));
    const auto slate       = platform::FindFunction(image, kPlaceSlate);
    const auto umg         = platform::FindFunction(image, kPlaceUmg);
    if (!system || !slate || !umg || !players::weak_object::Known())
    {
        platform::Log(L"popups: the placement code is not what the research read; popups stay player 1's in both halves");
        return false;
    }
    g_endUISystem = *system;
    g_placeSlate  = reinterpret_cast<PlaceFn>(*slate);
    g_placeUmg    = reinterpret_cast<PlaceFn>(*umg);

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

void Replay(int seat) noexcept
{
    const bool further = players::SeatIndex(seat);
    std::uintptr_t manager = 0;
    std::uintptr_t canvas[2]{};  // the manager's TSharedPtr, copied: the wrappers read only its object
    if (!g_installed || g_failed || g_record.count == 0 || !Read(g_endUISystem + kManager, manager) || manager == 0 ||
        !Read(manager + kCanvas, canvas) || canvas[0] == 0 || !Alive(canvas[1]))
    {
        return;
    }
    void* controller = further ? players::generation::ControllerOf(seat) : nullptr;
    if (further && controller == nullptr)
    {
        return;
    }
    int counts[3]{};
    {
        const Replaying<PopupRecord> replaying(g_record);  // the mod's own placements are not recorded
        for (std::size_t i = 0; i < std::size(kLists) && !g_failed; ++i)
        {
            counts[i] = ReplayList(manager, kLists[i], canvas, controller);
            g_failed  = counts[i] < 0;
        }
    }
    if (g_failed)
    {
        platform::Log(L"popups: a placement raised an exception; popups are no longer placed per view");
    }
    else if (further && counts[0] + counts[1] + counts[2] > 0 &&
             platform::Enabled(platform::Level::Trace))  // every HUD pass: Trace (platform/log.h)
    {
        platform::LogTrace(L"popups: placed again in {}'s view: {} numbers, {} texts, {} UMG ({} recorded)",
                           players::SeatName(seat), counts[0], counts[1], counts[2], g_record.count);
    }
}

void EndFrame() noexcept
{
    g_record.EndFrame();
}

}  // namespace lss::hud::popups
