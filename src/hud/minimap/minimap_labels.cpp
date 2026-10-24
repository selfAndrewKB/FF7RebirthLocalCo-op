#include "hud/minimap/minimap_labels.h"

#include "facts/engine.h"
#include "hud/minimap/label_relayouts.h"
#include "hud/minimap/label_states.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "players/seats.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::minimap_labels {

namespace {

namespace generation = player_two::generation;
using label_states::LabelAccess;
using label_states::LabelBook;
using label_states::LabelCounts;
using label_states::Parked;

// RE-p2-minimap-cost.md §7 (this build). SEndMapIcon::SetLabelText's one call into the text block's SetText:
// `mov rdx,rbp` (the FString*), `mov rcx,rdi` (the SEndTextBlock, never null here), the call, then only rsi/rbp/rdi and
// the stack are read (rax unread).
constexpr std::uint8_t   kSetLabelTextBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                 0x74, 0x24, 0x18, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x9A, 0x03,
                                                 0x72, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xB9, 0xC0, 0x03, 0x00, 0x00};
constexpr platform::FunctionFact kSetLabelText{L"SEndMapIcon__SetLabelText", 0x14098EB7C, kSetLabelTextBytes};
constexpr std::uint8_t   kSetTextArgsBytes[]  = {0x48, 0x8B, 0xD5, 0x48, 0x8B, 0xCF};
constexpr platform::FunctionFact kSetTextArgs{L"the SetText call's arguments", 0x14098EBAB, kSetTextArgsBytes};
constexpr std::uintptr_t kSetTextCall         = 0x14098EBB1;  // E8 rel32, inside the block at 0x14098EBB0
constexpr std::uint8_t   kSetTextAfterBytes[] = {0x80, 0xBE, 0x3C, 0x04, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSetTextAfter{L"after the SetText call", 0x14098EBB6, kSetTextAfterBytes};
// SEndTextBlock_SetText_RelayoutIfChanged: the game's own compare (≤ 20 characters) in front of its relayout.
constexpr std::uint8_t   kSetTextBytes[]      = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10,
                                                 0x48, 0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B,
                                                 0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xEB, 0x02, 0x72};
constexpr platform::FunctionFact kSetText{L"SEndTextBlock_SetText_RelayoutIfChanged", 0x14098EC24, kSetTextBytes};
constexpr std::uint8_t   kRelayoutBytes[]     = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
                                                 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x78, 0xF6, 0xFF, 0xFF, 0xB8};
// checked only: the state it writes is what is swapped
constexpr platform::FunctionFact kRelayout{L"the text block's relayout", 0x14106DA88, kRelayoutBytes};
// The text block's destructor frees the ranges with these (its offsets are checked below).
constexpr std::uint8_t   kArrayDtorBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                 0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
// TArray<TArray<Run52>>
constexpr platform::FunctionFact kRunsDtor{L"the runs array's destructor", 0x14098E3B4, kArrayDtorBytes};
// TArray<TArray<Quad80>>
constexpr platform::FunctionFact kQuadsDtor{L"the quads array's destructor", 0x14098E35C, kArrayDtorBytes};
constexpr std::uint8_t   kDtorQuadsLeaBytes[] = {0x48, 0x8D, 0x8B, 0xA0, 0x05, 0x00, 0x00};
// lea rcx,[rbx+5A0h]
constexpr platform::FunctionFact kDtorQuadsLea{L"the destructor's quads lea", 0x14098E221, kDtorQuadsLeaBytes};
constexpr std::uint8_t   kDtorRunsLeaBytes[]  = {0x48, 0x8D, 0x8B, 0x30, 0x05, 0x00, 0x00};
// lea rcx,[rbx+530h]
constexpr platform::FunctionFact kDtorRunsLea{L"the destructor's runs lea", 0x14098E25D, kDtorRunsLeaBytes};
constexpr std::uint8_t   kReleaseWeakBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0x54,
                                                 0x48, 0x01, 0x48, 0x2B, 0xE0, 0x83, 0x69, 0x0C};
constexpr platform::FunctionFact kReleaseWeak{L"RefController_ReleaseWeakReference", 0x140C29AAC, kReleaseWeakBytes};
constexpr std::uint8_t   kInvalidateBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
                                                 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0xB8};
constexpr platform::FunctionFact kInvalidate{L"SWidget::Invalidate", 0x140970724, kInvalidateBytes};
constexpr std::uint32_t  kInvalidateLayout    = 5;  // what the game's SetText passes after a relayout

// The text block and its MakeShared block: the reference controller sits 0x10 before the widget (shared count +8, weak
// count +0xC, not thread-safe), and TSharedFromThis keeps {widget, controller} at +0x8/+0x10.
constexpr std::uintptr_t kTextBlockVTable  = 0x1457936F0;
constexpr std::uintptr_t kPaintVTable      = 0x1457936C8;  // at +0x2B0
constexpr std::uintptr_t kGcObjectVTable   = 0x1457936D0;  // at +0x2B8
constexpr std::uintptr_t kControllerVTable = 0x1457936B8;
constexpr std::uint32_t  kControllerBefore = 0x10;
constexpr std::uint32_t  kWeakThisObject   = 0x08;
constexpr std::uint32_t  kWeakThisCounter  = 0x10;
constexpr std::uint32_t  kSharedCount      = 0x08;
constexpr std::uint32_t  kWeakCount        = 0x0C;
constexpr std::uint32_t  kPaintInterface   = 0x2B0;
constexpr std::uint32_t  kGcObject         = 0x2B8;
constexpr std::uint32_t  kGetterBound      = 0x2E8;  // the text attribute's getter (0: unbound)
constexpr std::uint32_t  kRelayoutLock     = 0x564;
constexpr std::uint32_t  kLanguageStamp    = 0x568;  // FName copied from a global (the language, §7.12)
constexpr std::uint32_t  kFont             = 0x2F0;  // the main and button font UObjects
constexpr std::uint32_t  kButtonFont       = 0x2F8;
constexpr std::uint32_t  kFastPathProxy    = 0x18;   // logged once (an invalidation root above the label)

constexpr ULONGLONG kCountsPeriodMs = 120'000;

using SetTextFn    = std::uint64_t (*)(void* textBlock, const void* text);
using ArrayDtorFn  = void (*)(void* array);
using ReleaseFn    = void (*)(void* controller);
using InvalidateFn = void (*)(void* widget, std::uint32_t reason);

platform::CallRedirect g_call;
// Game version 1.005 (RE-1005-hud-2.md): the call (0x1411A7E3D there) crosses a 16-byte block, so it is placed from
// the game thread, which alone runs it, outside the holder update (platform/deferred_sites.h: the session's tick),
// and only restored from there.
platform::DeferredSites g_placement;
std::uintptr_t         g_callSite       = 0;
std::uintptr_t         g_callTarget     = 0;
bool                   g_installed      = false;
bool                   g_refused        = false;  // a swap faulted: the labels as before until restart
int                    g_runSeat        = 0;      // the seat whose holder run is inside (0: none)
std::uintptr_t         g_textBlockVTable  = 0;
std::uintptr_t         g_paintVTable      = 0;
std::uintptr_t         g_gcObjectVTable   = 0;
std::uintptr_t         g_controllerVTable = 0;
ArrayDtorFn            g_quadsDtor      = nullptr;
ArrayDtorFn            g_runsDtor       = nullptr;
ArrayDtorFn            g_freeString     = nullptr;
ReleaseFn              g_releaseWeak    = nullptr;
InvalidateFn           g_invalidate     = nullptr;
const wchar_t*         g_lastRefusal    = nullptr;
bool                   g_loggedRefusal  = false;
bool                   g_loggedFirst    = false;
bool                   g_loggedInvalidateFault = false;
std::array<LabelCounts, players::kMaxPlayers> g_loggedCounts{};  // by seat ([0] unused)
ULONGLONG              g_countsAt       = 0;

using platform::Read;  // the guarded reads: platform/guarded.h

std::byte* ControllerOf(void* widget) noexcept
{
    return static_cast<std::byte*>(widget) - kControllerBefore;
}

// --- The engine side of the book (game thread) ---

bool Usable(void* widget) noexcept
{
    const std::byte* controller = ControllerOf(widget);
    std::uintptr_t   vtable = 0, paint = 0, gc = 0, weakObject = 0, weakCounter = 0, controllerVTable = 0;
    std::int32_t     shared = 0, getter = 0;
    std::uint8_t     lock   = 0;
    const wchar_t*   why    = nullptr;
    if (!Read(widget, 0, vtable) || vtable != g_textBlockVTable || !Read(widget, kPaintInterface, paint) ||
        paint != g_paintVTable || !Read(widget, kGcObject, gc) || gc != g_gcObjectVTable)
    {
        why = L"not an SEndTextBlock";
    }
    else if (!Read(widget, kWeakThisObject, weakObject) || weakObject != reinterpret_cast<std::uintptr_t>(widget) ||
             !Read(widget, kWeakThisCounter, weakCounter) || weakCounter != reinterpret_cast<std::uintptr_t>(controller) ||
             !Read(controller, 0, controllerVTable) || controllerVTable != g_controllerVTable)
    {
        why = L"not in its own MakeShared block";
    }
    else if (!Read(controller, kSharedCount, shared) || shared <= 0)
    {
        why = L"no shared reference";
    }
    else if (!Read(widget, kGetterBound, getter) || getter != 0)
    {
        why = L"its text has a bound getter";
    }
    else if (!Read(widget, kRelayoutLock, lock) || lock != 0)
    {
        why = L"its relayout is locked";
    }
    g_lastRefusal = why;
    return why == nullptr;
}

bool Alive(void* controller) noexcept
{
    std::int32_t shared = 0;
    return Read(controller, kSharedCount, shared) && shared > 0;
}

/// The four ranges exchanged with `parked`. Every read comes before any write, so a fault while reading changes
/// nothing; the writes go to the addresses just read, so they are not expected to fault.
bool Swap(void* widget, Parked& parked) noexcept
{
    auto* const w = static_cast<std::byte*>(widget);
    __try
    {
        Parked held;
        std::memcpy(held.text, w + label_states::kTextOffset, label_states::kTextSize);
        std::memcpy(held.runs, w + label_states::kRunsOffset, label_states::kRunsSize);
        std::memcpy(held.flags, w + label_states::kFlagsOffset, label_states::kFlagsSize);
        std::memcpy(held.quads, w + label_states::kQuadsOffset, label_states::kQuadsSize);
        std::memcpy(w + label_states::kTextOffset, parked.text, label_states::kTextSize);
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

void Invalidate(void* widget) noexcept
{
    if (!platform::CallGuarded(g_invalidate, widget, kInvalidateLayout) && !g_loggedInvalidateFault)
    {
        g_loggedInvalidateFault = true;
        platform::Log(L"minimap labels: SWidget::Invalidate raised an exception ({})",
                      platform::DescribeFault(platform::LastFault()).text);
    }
}

/// What a set-aside layout depends on outside the swapped ranges: the language stamp and both fonts.
std::uint64_t Stamp(void* widget) noexcept
{
    std::uint64_t language = 0, font = 0, buttonFont = 0;
    Read(widget, kLanguageStamp, language);
    Read(widget, kFont, font);
    Read(widget, kButtonFont, buttonFont);
    return language ^ std::rotl(font, 21) ^ std::rotl(buttonFont, 42);
}

bool HoldWeak(void* controller) noexcept
{
    std::int32_t weak = 0;
    if (!Read(controller, kWeakCount, weak) || weak <= 0)
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

/// The destructor's own helpers, in its order, on a set-aside state; then it is the empty state again.
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
        platform::Log(L"minimap labels: freeing a set-aside label state raised an exception ({})",
                      platform::DescribeFault(platform::LastFault()).text);
    }
    parked = label_states::EmptyState();
}

constexpr LabelAccess kAccess{&Usable, &Alive, &Swap, &Invalidate, &Stamp, &HoldWeak, &ReleaseWeak, &FreeParked};
// Each seat's label states, by seat ([0] unused): the seat's own text and layout per label.
static_assert(players::kMaxPlayers == 3, "one label book per player, listed below");
std::array<LabelBook, players::kMaxPlayers> g_books{LabelBook(kAccess), LabelBook(kAccess), LabelBook(kAccess)};

LabelBook& BookOf(int seat) noexcept
{
    return g_books[static_cast<std::size_t>(seat)];
}

void LogFirst(int seat, void* widget) noexcept
{
    g_loggedFirst = true;
    std::uintptr_t proxy = 0;
    Read(widget, kFastPathProxy, proxy);
    platform::Log(L"minimap labels: {}'s first label state swapped in ({} labels; fast-path proxy {:#x})",
                  players::SeatName(seat), BookOf(seat).Count(), proxy);
}

void LogCounts() noexcept
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_countsAt)
    {
        return;
    }
    g_countsAt = now + kCountsPeriodMs;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const LabelCounts& c = BookOf(seat).Counts();
        LabelCounts&       l = g_loggedCounts[static_cast<std::size_t>(seat)];
        if (c.swapsIn == l.swapsIn && c.refused == l.refused && c.faults == l.faults && c.released == l.released)
        {
            continue;
        }
        platform::LogDebug(L"minimap labels (2 min), {}: swaps in {} / out {}; labels {} (new {}, font resets {}, died "
                           L"in their pass {}, died aside {}, released {}); refused {}, table full {}, faults {}",
                           players::SeatName(seat), c.swapsIn - l.swapsIn, c.swapsOut - l.swapsOut,
                           BookOf(seat).Count(), c.created - l.created, c.fontResets - l.fontResets,
                           c.diedInside - l.diedInside, c.diedAside - l.diedAside, c.released - l.released,
                           c.refused - l.refused, c.full - l.full, c.faults - l.faults);
        l = c;
    }
}

// --- The redirected call ---

std::uint64_t SetText(void* textBlock, const void* text) noexcept
{
    const auto original = reinterpret_cast<SetTextFn>(g_call.Original());
    const int seat = g_runSeat;
    if (generation::OnGameThread() && seat != 0 && !g_refused && textBlock != nullptr)
    {
        LabelBook&          book          = BookOf(seat);
        const std::uint64_t refusedBefore = book.Counts().refused;
        if (!book.BeforeSetText(textBlock, ControllerOf(textBlock)))
        {
            g_refused = true;
            platform::Log(L"minimap labels: a label state swap raised an exception ({}); the labels are laid out per "
                          L"pass as before",
                          platform::DescribeFault(platform::LastFault()).text);
        }
        else if (book.Counts().refused != refusedBefore && !g_loggedRefusal)
        {
            g_loggedRefusal = true;
            platform::Log(L"minimap labels: a label was left shared ({}; {})", textBlock,
                          g_lastRefusal != nullptr ? g_lastRefusal : L"the table is full or its block is going away");
        }
        else if (!g_loggedFirst && book.Counts().swapsIn != 0)
        {
            LogFirst(seat, textBlock);
        }
    }
    // After the swap: what the game's own compare is about to see (hud/minimap/label_relayouts.h).
    const label_relayouts::Before before = label_relayouts::BeforeSetText(textBlock, text);
    const bool playerTwoState = before.read && seat != 0 && BookOf(seat).Holds(ControllerOf(textBlock));
    const std::uint64_t result = original(textBlock, text);
    label_relayouts::AfterSetText(textBlock, before, playerTwoState);
    return result;
}

/// The SetText call: at install, or on 1.005 from the game thread between its ticks, where it crosses a block.
bool PlaceCall(const platform::GameImage&) noexcept
{
    g_installed = g_placement.Place(g_call, g_callSite, g_callTarget, reinterpret_cast<std::uintptr_t>(&SetText));
    platform::Log(L"minimap labels: {}",
                  !g_installed ? L"the SetText call could not be redirected; each pass lays the labels out as before"
                  : g_placement.FromGameThread()
                      ? L"installed from the game thread (each player keeps their own label text and layout)"
                      : L"installed (each player keeps their own label text and layout)");
    return g_installed;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const bool checked    = platform::FindFunction(image, kSetLabelText) && platform::FindFunction(image, kRelayout) &&
                            platform::FindFunction(image, kSetTextArgs) &&
                            platform::FindFunction(image, kSetTextAfter) &&
                            platform::FindFunction(image, kDtorQuadsLea) && platform::FindFunction(image, kDtorRunsLea);
    const auto callAt     = image.Rebase(kSetTextCall, 5);
    const auto setText    = platform::FindFunction(image, kSetText);
    const auto quads      = platform::FindFunction(image, kQuadsDtor);
    const auto runs       = platform::FindFunction(image, kRunsDtor);
    const auto freeString = platform::FindFunction(image, facts::kFreeArrayData);
    const auto release    = platform::FindFunction(image, kReleaseWeak);
    const auto invalidate = platform::FindFunction(image, kInvalidate);
    const auto textBlock  = image.Rebase(kTextBlockVTable, sizeof(void*));
    const auto paint      = image.Rebase(kPaintVTable, sizeof(void*));
    const auto gcObject   = image.Rebase(kGcObjectVTable, sizeof(void*));
    const auto controller = image.Rebase(kControllerVTable, sizeof(void*));
    if (!checked || !callAt || !setText || !quads || !runs || !freeString || !release || !invalidate || !textBlock ||
        !paint || !gcObject || !controller)
    {
        platform::Log(L"minimap labels: the label code is not what the research read; each pass lays the labels out as "
                      L"before");
        return false;
    }
    g_textBlockVTable  = *textBlock;
    g_paintVTable      = *paint;
    g_gcObjectVTable   = *gcObject;
    g_controllerVTable = *controller;
    g_quadsDtor        = reinterpret_cast<ArrayDtorFn>(*quads);
    g_runsDtor         = reinterpret_cast<ArrayDtorFn>(*runs);
    g_freeString       = reinterpret_cast<ArrayDtorFn>(*freeString);
    g_releaseWeak      = reinterpret_cast<ReleaseFn>(*release);
    g_invalidate       = reinterpret_cast<InvalidateFn>(*invalidate);
    g_callSite   = *callAt;
    g_callTarget = *setText;
    const bool crosses = platform::DeferredSites::Deferred() && (*callAt & 0xF) + 5 > 16;
    const bool placed  = g_placement.Install(image, &PlaceCall, crosses);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"minimap labels: {}",
                      placed ? L"checked; the SetText call follows on the game thread"
                             : L"no room to place the SetText call from the game thread; each pass lays the labels out "
                               L"as before");
    }
    return placed;
}

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    // A call crossing two blocks is restored only from the game thread; elsewhere it stays, passing through (no run).
    const bool restorable = !g_placement.FromGameThread() || generation::OnGameThread();
    if (restorable && g_call.IsInstalled() && !g_call.Restore())
    {
        platform::Log(L"minimap labels: the SetText call was changed after us; left in place (it passes through)");
    }
    std::size_t left = 0;
    for (LabelBook& book : g_books)
    {
        if (g_installed && generation::OnGameThread())
        {
            book.ReleaseAll();
        }
        left += book.Count();
    }
    if (left != 0)
    {
        // The counts are not thread-safe and the widgets are the game thread's: off it, the records are left (each
        // widget holds player 1's state outside the passes; the seats' set-aside copies and weak references leak).
        platform::Log(L"minimap labels: removed off the game thread; {} label record(s) left unreleased", left);
    }
    g_installed = false;
    g_runSeat   = 0;
}

void BeginRun(int seat) noexcept
{
    g_runSeat = g_installed && players::SeatIndex(seat) ? seat : 0;
    label_relayouts::SetScope(label_relayouts::Scope::PlayerTwoRun);
}

void EndRun() noexcept
{
    g_runSeat = 0;
    label_relayouts::SetScope(label_relayouts::Scope::Other);
}

void SwapBack() noexcept
{
    if (g_installed && generation::OnGameThread())
    {
        for (LabelBook& book : g_books)
        {
            book.SwapBack();  // only the book of the seat whose paint ran has states inside
        }
    }
}

void Tick(unsigned seatsOn) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        LabelBook& book = BookOf(seat);
        if ((seatsOn & (1u << seat)) != 0 && !g_refused)
        {
            book.Sweep();
        }
        else if (book.Count() != 0)
        {
            book.ReleaseAll();
        }
    }
    LogCounts();
}

}  // namespace lss::hud::minimap_labels
