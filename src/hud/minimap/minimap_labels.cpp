#include "hud/minimap/minimap_labels.h"

#include "hud/label_states.h"
#include "hud/minimap/label_relayouts.h"
#include "hud/text_blocks.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::minimap_labels {

namespace {

namespace generation = players::generation;
using label_states::LabelAccess;
using label_states::LabelBook;
using label_states::LabelCounts;

// research/RE-p2-minimap-cost.md (this build). SEndMapIcon::SetLabelText's one call into the text block's SetText:
// `mov rdx,rbp` (the FString*), `mov rcx,rdi` (the SEndTextBlock, never null here), the call, then only rsi/rbp/rdi and
// the stack are read (rax unread).
constexpr std::uint8_t   kSetLabelTextBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                 0x74, 0x24, 0x18, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x5E, 0x3D,
                                                 0xFC, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xB9, 0xC0, 0x03, 0x00, 0x00};
constexpr platform::FunctionFact kSetLabelText{L"SEndMapIcon__SetLabelText", 0x1411A7E08, kSetLabelTextBytes};
constexpr std::uint8_t   kSetTextArgsBytes[]  = {0x48, 0x8B, 0xD5, 0x48, 0x8B, 0xCF};
constexpr platform::FunctionFact kSetTextArgs{L"the SetText call's arguments", 0x1411A7E37, kSetTextArgsBytes};
constexpr std::uintptr_t kSetTextCall         = 0x1411A7E3D;  // E8 rel32, inside one 16-byte block
constexpr std::uint8_t   kSetTextAfterBytes[] = {0x80, 0xBE, 0x3C, 0x04, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSetTextAfter{L"after the SetText call", 0x1411A7E42, kSetTextAfterBytes};
// SEndTextBlock_SetText_RelayoutIfChanged: the game's own compare (≤ 20 characters) in front of its relayout.
constexpr std::uint8_t   kSetTextBytes[]      = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10,
                                                 0x48, 0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B,
                                                 0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xAF, 0x3C, 0xFC};
constexpr platform::FunctionFact kSetText{L"SEndTextBlock_SetText_RelayoutIfChanged", 0x1411A7EB0, kSetTextBytes};
constexpr std::uint32_t  kFastPathProxy    = 0x18;   // logged once (an invalidation root above the label)

constexpr ULONGLONG kCountsPeriodMs = 120'000;

using SetTextFn = std::uint64_t (*)(void* textBlock, const void* text);

platform::CallRedirect g_call;
// Game version 1.005 (research/RE-1005-hud-2.md): the call crosses a 16-byte block, so it is placed from
// the game thread, which alone runs it, outside the holder update (platform/deferred_sites.h: the session's tick),
// and only restored from there.
platform::DeferredSites g_placement;
std::uintptr_t         g_callSite       = 0;
std::uintptr_t         g_callTarget     = 0;
bool                   g_installed      = false;
bool                   g_refused        = false;  // a swap faulted: no swaps until restart
int                    g_runSeat        = 0;      // the seat whose holder run is inside (0: none)
const wchar_t*         g_lastRefusal    = nullptr;
bool                   g_loggedRefusal  = false;
bool                   g_loggedFirst    = false;
std::array<LabelCounts, players::kMaxPlayers> g_loggedCounts{};  // by seat ([0] unused)
ULONGLONG              g_countsAt       = 0;

void* ControllerOf(void* widget) noexcept
{
    return text_blocks::ControllerOf(widget);
}

/// A label carries a seat's text and layout when it can carry a state at all, its text is its own (no bound getter)
/// and its relayout is not locked (the game's SetText lays the seat's state out at once).
bool Usable(void* widget) noexcept
{
    std::int32_t   getter = 0;
    std::uint8_t   lock   = 0;
    const wchar_t* why    = text_blocks::Unswappable(widget);
    if (why == nullptr && (!platform::Read(widget, text_blocks::kGetterBound, getter) || getter != 0))
    {
        why = L"its text has a bound getter";
    }
    else if (why == nullptr && (!platform::Read(widget, text_blocks::kRelayoutLock, lock) || lock != 0))
    {
        why = L"its relayout is locked";
    }
    g_lastRefusal = why;
    return why == nullptr;
}

constexpr LabelAccess kAccess{&Usable,
                              &text_blocks::Alive,
                              &text_blocks::SwapState,
                              &text_blocks::Invalidate,
                              &text_blocks::Stamp,
                              &text_blocks::HoldWeak,
                              &text_blocks::ReleaseWeak,
                              &text_blocks::FreeParked};
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
    platform::Read(widget, kFastPathProxy, proxy);
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
        if (!book.SwapIn(textBlock, ControllerOf(textBlock)))
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
    const bool seatState = before.read && seat != 0 && BookOf(seat).Holds(ControllerOf(textBlock));
    const std::uint64_t result = original(textBlock, text);
    label_relayouts::AfterSetText(textBlock, before, seatState);
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
    const bool checked = platform::FindFunction(image, kSetLabelText) && platform::FindFunction(image, kSetTextArgs) &&
                         platform::FindFunction(image, kSetTextAfter);
    const auto callAt  = image.Rebase(kSetTextCall, 5);
    const auto setText = platform::FindFunction(image, kSetText);
    if (!checked || !callAt || !setText || !text_blocks::Install(image))
    {
        platform::Log(L"minimap labels: the label code is not what the research read; each pass lays the labels out as "
                      L"before");
        return false;
    }
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
    label_relayouts::SetScope(label_relayouts::Scope::Run, seat);
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
