#include "hud/minimap/minimap_icons.h"

#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::minimap_icons {

namespace {

namespace generation = players::generation;

// The holder update's icon loop (research/RE-p2-minimap-cost.md). Both calls take `mov rdx,[r15+r12]` (the source's
// FName key, a qword) and `mov rcx,rdi` (the holder); neither result is read. Each call's five bytes cross a 16-byte
// block.
constexpr std::uintptr_t kUpdateCall          = 0x140B7C399;  // in range: AreaMapIcon_Update(holder, key, source)
constexpr std::uint8_t   kUpdateBytes[]       = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x48,
                                                 0x89, 0x78, 0x20, 0x48, 0x89, 0x50, 0x10, 0x55, 0x41, 0x54, 0x41, 0x55};
constexpr platform::FunctionFact kUpdate{L"AreaMapIcon_Update", 0x140DD2AD0, kUpdateBytes};
constexpr std::uint8_t   kUpdateArgsBytes[]   = {0x4B, 0x8B, 0x14, 0x27, 0x4C, 0x8B, 0xC6, 0x48, 0x8B, 0xCF};
constexpr platform::FunctionFact kUpdateArgs{L"the update call's arguments", 0x140B7C38F, kUpdateArgsBytes};
constexpr std::uint8_t   kUpdateAfterBytes[]  = {0xE9, 0x08, 0xFF, 0xFF, 0xFF};
constexpr platform::FunctionFact kUpdateAfter{L"after the update call", 0x140B7C39E, kUpdateAfterBytes};
constexpr std::uintptr_t kRemoveCall          = 0x140B7C2A6;  // out of range: NaviMapHolder_RemoveIconEntry_DestroysWidget
constexpr std::uint8_t   kRemoveBytes[]       = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
                                                 0x56, 0x57, 0x41, 0x56, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kRemove{L"NaviMapHolder_RemoveIconEntry_DestroysWidget", 0x14078BFD0, kRemoveBytes};
constexpr std::uint8_t   kRemoveArgsBytes[]   = {0x4B, 0x8B, 0x14, 0x27, 0x48, 0x8B, 0xCF};
constexpr platform::FunctionFact kRemoveArgs{L"the remove call's arguments", 0x140B7C29F, kRemoveArgsBytes};
constexpr std::uint8_t   kRemoveAfterBytes[]  = {0x8A, 0x9C, 0x24, 0x81, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRemoveAfter{L"after the remove call", 0x140B7C2AB, kRemoveAfterBytes};

// The holder's entries: a TSet at holder+0xE8 of 160-byte elements, the FName key at +0, the entry after it:
// entry+0x8 the SEndMapIcon, entry+0x1C the icon kind. The widget's Slate visibility byte at +0x1B0 (stock 4.26
// EVisibility: bit 0 = painted).
constexpr std::uint32_t kHolderEntries  = 0xE8;
constexpr std::uint32_t kEntryElement   = 160;
constexpr std::uint32_t kEntry          = 0x08;
constexpr std::uint32_t kEntryIcon      = 0x08;
constexpr std::uint32_t kEntryKind      = 0x1C;
constexpr std::uint32_t kIconVisibility = 0x1B0;
constexpr std::uint8_t  kPainted        = 0x01;

constexpr ULONGLONG   kCountsPeriodMs = 120'000;
constexpr std::size_t kMaxKept        = 4096;  // per run; far above any icon count
constexpr std::size_t kMaxReported    = 64;    // shown-anyway keys reported once each

using UpdateFn = void (*)(void* holder, std::uint64_t key, const void* source);
using RemoveFn = void (*)(void* holder, std::uint64_t key);

struct Counts
{
    std::uint64_t runs = 0, kept = 0, removes = 0, skipped = 0, shown = 0;
};

platform::CallRedirect g_updateCall;
platform::CallRedirect g_removeCall;
platform::DeferredSites g_placement;  // the two calls cross a block: placed from the game thread, off the holder update
std::uintptr_t          g_updateSite = 0, g_updateTarget = 0, g_removeSite = 0, g_removeTarget = 0;

// Game thread.
constexpr int kPlayers = players::kMaxPlayers;

bool Contains(const std::vector<std::uint64_t>& keys, std::uint64_t key) noexcept
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

std::atomic<bool>          g_active{false};    // a seat's minimap is on (Remove may write it off the game thread)
int                        g_run    = 0;       // the seat inside the mod's seat run; 0 otherwise (player 1's centre)
bool                       g_inModRun = false;
std::vector<std::uint64_t> g_kept[kPlayers];   // the keys each player's last run kept, by seat
bool                       g_keptValid[kPlayers] = {};
std::vector<std::uint64_t> g_skippedThisRun;
std::vector<std::uint64_t> g_reported;          // shown-anyway keys already logged
Counts                     g_counts[kPlayers], g_logged[kPlayers];
ULONGLONG                  g_countsAt     = 0;
bool                       g_loggedFirst  = false;

const wchar_t* RunName(int run) noexcept
{
    return players::SeatName(run);
}

/// Another player's last run kept `key` (-1: none did).
int KeptByOther(int run, std::uint64_t key) noexcept
{
    for (int other = 0; other < kPlayers; ++other)
    {
        if (other != run && g_keptValid[other] && Contains(g_kept[other], key))
        {
            return other;
        }
    }
    return -1;
}


/// The holder's AreaMapIcon_Update call: this run keeps `key` (the game's own tests passed).
void Update(void* holder, std::uint64_t key, const void* source) noexcept
{
    const auto original = reinterpret_cast<UpdateFn>(g_updateCall.Original());
    if (g_active && g_inModRun && generation::OnGameThread())
    {
        std::vector<std::uint64_t>& kept = g_kept[g_run];
        if (kept.size() < kMaxKept)
        {
            try
            {
                kept.push_back(key);
            }
            catch (...)
            {
            }
        }
        ++g_counts[g_run].kept;
    }
    original(holder, key, source);
}

/// The holder's remove call: skipped for a key another player's last run kept.
void RemoveIcon(void* holder, std::uint64_t key) noexcept
{
    const auto original = reinterpret_cast<RemoveFn>(g_removeCall.Original());
    if (g_active && generation::OnGameThread())
    {
        const int run   = g_run;
        const int other = KeptByOther(run, key);
        ++g_counts[run].removes;
        if (other >= 0)
        {
            ++g_counts[run].skipped;
            if (g_skippedThisRun.size() < kMaxKept)
            {
                try
                {
                    g_skippedThisRun.push_back(key);
                }
                catch (...)
                {
                }
            }
            if (!g_loggedFirst)
            {
                g_loggedFirst = true;
                platform::LogDebug(L"minimap icons: {}'s run kept an icon {}'s map has in range (key {:#x}) instead of "
                                   L"removing it",
                                   RunName(run), RunName(other), key);
            }
            return;
        }
    }
    original(holder, key);
}

/// After a mod run: a kept icon this run's render loop left painted (the range hide does not cover every kind)
/// would show the other player's icon on this player's map. Logged once per key.
void CheckShown(const void* holder) noexcept
{
    if (g_skippedThisRun.empty() || holder == nullptr)
    {
        return;
    }
    const int   run       = g_run;
    std::size_t remaining = g_skippedThisRun.size();  // the walk stops once every skipped key was seen
    try
    {
        platform::ForEachSparseElement(
            reinterpret_cast<std::uintptr_t>(holder) + kHolderEntries, kEntryElement, [&](std::uintptr_t element) {
                std::uint64_t key = 0;
                if (!platform::CopyGuarded(&key, reinterpret_cast<const void*>(element), sizeof(key)) ||
                    !Contains(g_skippedThisRun, key))
                {
                    return true;
                }
                --remaining;
                const auto*  entry      = reinterpret_cast<const std::byte*>(element + kEntry);
                std::uintptr_t icon     = 0;
                std::uint8_t kind       = 0, visibility = 0;
                platform::CopyGuarded(&kind, entry + kEntryKind, sizeof(kind));
                if (!platform::CopyGuarded(&icon, entry + kEntryIcon, sizeof(icon)) || icon == 0 ||
                    !platform::CopyGuarded(&visibility, reinterpret_cast<const void*>(icon + kIconVisibility),
                                           sizeof(visibility)) ||
                    (visibility & kPainted) == 0)
                {
                    return remaining != 0;
                }
                ++g_counts[run].shown;
                if (!Contains(g_reported, key) && g_reported.size() < kMaxReported)
                {
                    g_reported.push_back(key);
                    platform::Log(L"minimap icons: an icon kept for another player is shown on {}'s map (key {:#x}, "
                                  L"kind {}, visibility {:#x})",
                                  RunName(run), key, kind, visibility);
                }
                return remaining != 0;
            });
    }
    catch (...)
    {
    }
}

void LogCounts() noexcept
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_countsAt)
    {
        return;
    }
    g_countsAt = now + kCountsPeriodMs;
    Counts d[kPlayers];
    bool   any = false;
    for (int i = 0; i < kPlayers; ++i)
    {
        d[i] = {g_counts[i].runs - g_logged[i].runs,       g_counts[i].kept - g_logged[i].kept,
                g_counts[i].removes - g_logged[i].removes, g_counts[i].skipped - g_logged[i].skipped,
                g_counts[i].shown - g_logged[i].shown};
        g_logged[i] = g_counts[i];
        any         = any || d[i].removes != 0 || d[i].kept != 0;
    }
    if (!any || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        std::wstring runs;
        for (int i = 0; i < kPlayers; ++i)
        {
            if (i == 0 || d[i].runs != 0)
            {
                runs += std::format(L"{}{}'s {} {}: kept {}, removes {}, skipped for another player {}, shown anyway {}",
                                    i == 0 ? L"" : L"; ", RunName(i), i == 0 ? L"centre: mod runs" : L"runs",
                                    d[i].runs, d[i].kept, d[i].removes, d[i].skipped, d[i].shown);
            }
        }
        platform::LogDebug(L"minimap icons (2 min): {}", runs);
    }
    catch (...)
    {
    }
}

void Clear() noexcept
{
    for (int i = 0; i < kPlayers; ++i)
    {
        g_kept[i].clear();
        g_keptValid[i] = false;
    }
    g_skippedThisRun.clear();
}

/// The two calls, from the game thread outside the holder update (they cross a block on both builds).
bool PlaceCalls(const platform::GameImage&) noexcept
{
    const bool update = g_placement.Place(g_updateCall, g_updateSite, g_updateTarget,
                                          reinterpret_cast<std::uintptr_t>(&Update));
    const bool remove = update && g_placement.Place(g_removeCall, g_removeSite, g_removeTarget,
                                                    reinterpret_cast<std::uintptr_t>(&RemoveIcon));
    if (!remove)
    {
        if (update)
        {
            g_updateCall.Restore();
        }
        platform::Log(L"minimap icons: the holder's icon calls could not be redirected; each run removes the other "
                      L"player's icons as before");
        return false;
    }
    platform::Log(L"minimap icons: installed (each player's minimap keeps the icons another player's has in range)");
    return true;
}

}  // namespace

bool Prepare(const platform::GameImage& image) noexcept
{
    const bool sites    = platform::FindFunction(image, kUpdateArgs) && platform::FindFunction(image, kUpdateAfter) &&
                          platform::FindFunction(image, kRemoveArgs) && platform::FindFunction(image, kRemoveAfter);
    const auto updateAt = image.Rebase(kUpdateCall, 5);
    const auto update   = platform::FindFunction(image, kUpdate);
    const auto removeAt = image.Rebase(kRemoveCall, 5);
    const auto remove   = platform::FindFunction(image, kRemove);
    if (!sites || !updateAt || !update || !removeAt || !remove)
    {
        platform::Log(L"minimap icons: the holder's icon calls are not what the research read; each run removes the "
                      L"other players' icons as before");
        return false;
    }
    g_updateSite   = *updateAt;
    g_updateTarget = *update;
    g_removeSite   = *removeAt;
    g_removeTarget = *remove;
    return g_placement.Install(image, &PlaceCalls, true);  // outside the holder update: the session's tick
}

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    g_active = false;
    if (!generation::OnGameThread())
    {
        return;  // they cross a block: restored only from the game thread; inactive, they pass through
    }
    for (platform::CallRedirect* call : {&g_removeCall, &g_updateCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"minimap icons: a call was changed after us; left in place (it passes through)");
        }
    }
    Clear();
}

void Tick(unsigned seatsOn) noexcept
{
    if (!g_removeCall.IsInstalled() || !generation::OnGameThread())
    {
        return;
    }
    const bool on = seatsOn != 0;
    if (on != g_active)
    {
        g_active = on;
        Clear();  // off: the next run centred on player 1 removes the seats' icons, as the game does
    }
    for (int seat = 1; on && seat < kPlayers; ++seat)
    {
        if ((seatsOn & (1u << seat)) == 0 && g_keptValid[seat])
        {
            g_kept[seat].clear();  // a seat whose minimap is off keeps nothing for the others
            g_keptValid[seat] = false;
        }
    }
    LogCounts();
}

void BeginRun(int run) noexcept
{
    if (!g_active || !generation::OnGameThread())
    {
        return;
    }
    g_run      = run >= 0 && run < kPlayers ? run : 0;
    g_inModRun = true;
    g_kept[g_run].clear();
    g_keptValid[g_run] = true;
    g_skippedThisRun.clear();
    ++g_counts[g_run].runs;
}

void EndRun(const void* holder) noexcept
{
    if (!g_inModRun || !generation::OnGameThread())
    {
        return;
    }
    CheckShown(holder);
    g_skippedThisRun.clear();
    g_run      = 0;
    g_inModRun = false;
}

}  // namespace lss::hud::minimap_icons
