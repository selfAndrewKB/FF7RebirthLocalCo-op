#include "hud/text_blocks.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <bit>
#include <cstring>
#include <initializer_list>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::text_blocks {

namespace {

using label_states::Parked;

// research/RE-p2-minimap-cost.md, RE-per-player-icons.md (this build).
constexpr std::uint8_t kRelayoutBytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                           0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD8, 0xFE, 0xFF, 0xFF, 0xB8,
                                           0x28, 0x02, 0x00, 0x00, 0xE8, 0xB1, 0x9D, 0x04, 0x01, 0x48};
// SEndTextBlock_Relayout_MeasureLines (block): lays the text out from the block's fields alone.
constexpr platform::FunctionFact kRelayout{L"the text block's relayout", 0x141121DB0, kRelayoutBytes};
// The text block's destructor frees the ranges with these (its offsets are checked below).
constexpr std::uint8_t   kArrayDtorBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                 0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
// TArray<TArray<Run52>>
constexpr platform::FunctionFact kRunsDtor{L"the runs array's destructor", 0x14088675C, kArrayDtorBytes};
// TArray<TArray<Quad80>>
constexpr platform::FunctionFact kQuadsDtor{L"the quads array's destructor", 0x140886704, kArrayDtorBytes};
constexpr std::uint8_t   kDtorQuadsLeaBytes[] = {0x48, 0x8D, 0x8B, 0xA0, 0x05, 0x00, 0x00};
// lea rcx,[rbx+5A0h]
constexpr platform::FunctionFact kDtorQuadsLea{L"the destructor's quads lea", 0x1408865C9, kDtorQuadsLeaBytes};
constexpr std::uint8_t   kDtorRunsLeaBytes[]  = {0x48, 0x8D, 0x8B, 0x30, 0x05, 0x00, 0x00};
// lea rcx,[rbx+530h]
constexpr platform::FunctionFact kDtorRunsLea{L"the destructor's runs lea", 0x140886605, kDtorRunsLeaBytes};
constexpr std::uint8_t   kReleaseWeakBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xE2, 0x61,
                                                 0x39, 0x01, 0x48, 0x2B, 0xE0, 0x83, 0x69, 0x0C};
constexpr platform::FunctionFact kReleaseWeak{L"RefController_ReleaseWeakReference", 0x140DD5994, kReleaseWeakBytes};
constexpr std::uint8_t   kInvalidateBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
                                                 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0xB8};
constexpr platform::FunctionFact kInvalidate{L"SWidget::Invalidate", 0x140B12B74, kInvalidateBytes};
constexpr std::uint32_t  kInvalidateLayout    = 5;  // what the game's text setters pass after a relayout
// The game's set of live blocks (a TSet<SEndTextBlock*>): the block's constructor adds it, its destructor removes it.
constexpr std::uintptr_t kLiveSetGlobal       = 0x1486504C0;

// The text block and its MakeShared block: the reference controller sits 0x10 before the widget (shared count +8, weak
// count +0xC, not thread-safe), and TSharedFromThis keeps {widget, controller} at +0x8/+0x10.
constexpr std::uintptr_t kTextBlockVTable  = 0x1455EA0C8;
constexpr std::uintptr_t kPaintVTable      = 0x1455E9E08;  // at +0x2B0
constexpr std::uintptr_t kGcObjectVTable   = 0x1455E9E10;  // at +0x2B8
constexpr std::uintptr_t kControllerVTable = 0x1455E9DF8;
constexpr std::uint32_t  kControllerBefore = 0x10;
constexpr std::uint32_t  kWeakThisObject   = 0x08;
constexpr std::uint32_t  kWeakThisCounter  = 0x10;
constexpr std::uint32_t  kSharedCount      = 0x08;
constexpr std::uint32_t  kWeakCount        = 0x0C;
constexpr std::uint32_t  kPaintInterface   = 0x2B0;
constexpr std::uint32_t  kGcObject         = 0x2B8;

using RelayoutFn   = void (*)(void* block);
using ArrayDtorFn  = void (*)(void* array);
using ReleaseFn    = void (*)(void* controller);
using InvalidateFn = void (*)(void* widget, std::uint32_t reason);

// Set once at install.
bool           g_installed        = false;
std::uintptr_t g_textBlockVTable  = 0;
std::uintptr_t g_paintVTable      = 0;
std::uintptr_t g_gcObjectVTable   = 0;
std::uintptr_t g_controllerVTable = 0;
std::uintptr_t g_liveSet          = 0;
RelayoutFn     g_relayout         = nullptr;
ArrayDtorFn    g_quadsDtor        = nullptr;
ArrayDtorFn    g_runsDtor         = nullptr;
ArrayDtorFn    g_freeString       = nullptr;
ReleaseFn      g_releaseWeak      = nullptr;
InvalidateFn   g_invalidate       = nullptr;

// Game thread.
bool g_loggedInvalidateFault = false;

/// The ranges exchanged with `parked`, the text's with `withText`. Every read comes before any write, so a fault while
/// reading changes nothing; the writes go to the addresses just read, so they are not expected to fault.
bool Exchange(void* block, Parked& parked, bool withText) noexcept
{
    auto* const w = static_cast<std::byte*>(block);
    __try
    {
        Parked held = parked;
        if (withText)
        {
            std::memcpy(held.text, w + label_states::kTextOffset, label_states::kTextSize);
        }
        std::memcpy(held.runs, w + label_states::kRunsOffset, label_states::kRunsSize);
        std::memcpy(held.flags, w + label_states::kFlagsOffset, label_states::kFlagsSize);
        std::memcpy(held.quads, w + label_states::kQuadsOffset, label_states::kQuadsSize);
        if (withText)
        {
            std::memcpy(w + label_states::kTextOffset, parked.text, label_states::kTextSize);
        }
        std::memcpy(w + label_states::kRunsOffset, parked.runs, label_states::kRunsSize);
        std::memcpy(w + label_states::kFlagsOffset, parked.flags, label_states::kFlagsSize);
        std::memcpy(w + label_states::kQuadsOffset, parked.quads, label_states::kQuadsSize);
        parked = held;
        return true;
    }
    __except (platform::GuardFilterRecording(GetExceptionInformation()))
    {
        return false;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_installed)
    {
        return true;
    }
    const bool checked    = platform::FindFunction(image, kDtorQuadsLea) && platform::FindFunction(image, kDtorRunsLea);
    const auto relayout   = platform::FindFunction(image, kRelayout);
    const auto quads      = platform::FindFunction(image, kQuadsDtor);
    const auto runs       = platform::FindFunction(image, kRunsDtor);
    const auto freeString = platform::FindFunction(image, facts::kFreeArrayData);
    const auto release    = platform::FindFunction(image, kReleaseWeak);
    const auto invalidate = platform::FindFunction(image, kInvalidate);
    const auto textBlock  = image.Rebase(kTextBlockVTable, sizeof(void*));
    const auto paint      = image.Rebase(kPaintVTable, sizeof(void*));
    const auto gcObject   = image.Rebase(kGcObjectVTable, sizeof(void*));
    const auto controller = image.Rebase(kControllerVTable, sizeof(void*));
    const auto liveSet    = image.Rebase(kLiveSetGlobal, platform::kSparseNumBits + sizeof(std::int32_t));
    if (!checked || !relayout || !quads || !runs || !freeString || !release || !invalidate || !textBlock || !paint ||
        !gcObject || !controller || !liveSet)
    {
        return false;
    }
    g_textBlockVTable  = *textBlock;
    g_paintVTable      = *paint;
    g_gcObjectVTable   = *gcObject;
    g_controllerVTable = *controller;
    g_liveSet          = *liveSet;
    g_relayout         = reinterpret_cast<RelayoutFn>(*relayout);
    g_quadsDtor        = reinterpret_cast<ArrayDtorFn>(*quads);
    g_runsDtor         = reinterpret_cast<ArrayDtorFn>(*runs);
    g_freeString       = reinterpret_cast<ArrayDtorFn>(*freeString);
    g_releaseWeak      = reinterpret_cast<ReleaseFn>(*release);
    g_invalidate       = reinterpret_cast<InvalidateFn>(*invalidate);
    g_installed        = true;
    return true;
}

bool Installed() noexcept
{
    return g_installed;
}

void* ControllerOf(void* block) noexcept
{
    return static_cast<std::byte*>(block) - kControllerBefore;
}

const wchar_t* Unswappable(void* block) noexcept
{
    const void*    controller = ControllerOf(block);
    std::uintptr_t vtable = 0, paint = 0, gc = 0, weakObject = 0, weakCounter = 0, controllerVTable = 0;
    if (!g_installed || !platform::Read(block, 0, vtable) || vtable != g_textBlockVTable ||
        !platform::Read(block, kPaintInterface, paint) || paint != g_paintVTable ||
        !platform::Read(block, kGcObject, gc) || gc != g_gcObjectVTable)
    {
        return L"not an SEndTextBlock";
    }
    if (!platform::Read(block, kWeakThisObject, weakObject) || weakObject != reinterpret_cast<std::uintptr_t>(block) ||
        !platform::Read(block, kWeakThisCounter, weakCounter) ||
        weakCounter != reinterpret_cast<std::uintptr_t>(controller) || !platform::Read(controller, 0, controllerVTable) ||
        controllerVTable != g_controllerVTable)
    {
        return L"not in its own MakeShared block";
    }
    if (!Alive(ControllerOf(block)))
    {
        return L"no shared reference";
    }
    return nullptr;
}

bool Alive(void* controller) noexcept
{
    std::int32_t shared = 0;
    return platform::Read(controller, kSharedCount, shared) && shared > 0;
}

bool SwapState(void* block, Parked& parked) noexcept
{
    return Exchange(block, parked, true);
}

bool SwapLayout(void* block, Parked& parked) noexcept
{
    return Exchange(block, parked, false);
}

void Invalidate(void* block) noexcept
{
    if (!platform::CallGuarded(g_invalidate, block, kInvalidateLayout) && !g_loggedInvalidateFault)
    {
        g_loggedInvalidateFault = true;
        platform::Log(L"text blocks: SWidget::Invalidate raised an exception ({})",
                      platform::DescribeFault(platform::LastFault()).text);
    }
}

std::uint64_t Stamp(void* block) noexcept
{
    std::uint64_t language = 0, font = 0, buttonFont = 0;
    platform::Read(block, kLanguageStamp, language);
    platform::Read(block, kFont, font);
    platform::Read(block, kButtonFont, buttonFont);
    return language ^ std::rotl(font, 21) ^ std::rotl(buttonFont, 42);
}

bool HoldWeak(void* controller) noexcept
{
    std::int32_t weak = 0;
    if (!platform::Read(controller, kWeakCount, weak) || weak <= 0)
    {
        return false;  // the widget's own WeakThis holds one: zero would be a block already going away
    }
    ++weak;
    return platform::WriteGuarded(static_cast<std::byte*>(controller) + kWeakCount, &weak, sizeof(weak));
}

void ReleaseWeak(void* controller) noexcept
{
    platform::CallGuarded(g_releaseWeak, controller);
}

void FreeParked(Parked& parked) noexcept
{
    bool freed = true;
    for (std::size_t at : {0x30u, 0x20u, 0x10u, 0x00u})  // +0x5A0, +0x590, +0x580, +0x570
    {
        freed = platform::CallGuarded(g_quadsDtor, static_cast<void*>(parked.quads + at)) && freed;
    }
    for (std::size_t at : {0x10u, 0x00u})  // +0x540, +0x530
    {
        freed = platform::CallGuarded(g_runsDtor, static_cast<void*>(parked.runs + at)) && freed;
    }
    freed = platform::CallGuarded(g_freeString, static_cast<void*>(parked.text)) && freed;
    if (!freed)
    {
        platform::Log(L"text blocks: freeing a set-aside state raised an exception ({})",
                      platform::DescribeFault(platform::LastFault()).text);
    }
    parked = label_states::EmptyState();
}

bool Relayout(void* block) noexcept
{
    if (!g_installed || !platform::CallGuarded(g_relayout, block))
    {
        return false;
    }
    Invalidate(block);
    return true;
}

std::uintptr_t LiveSet() noexcept
{
    return g_liveSet;
}

}  // namespace lss::hud::text_blocks
