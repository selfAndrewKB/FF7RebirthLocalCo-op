#include "player_two/charapop_centre.h"

#include "facts/controller.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/generation.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <utility>

namespace lss::player_two::charapop_centre {

namespace {

using Clock = std::chrono::steady_clock;

// CharaPop_SetOverrideCenter(bool enable, const FVector* centre): writes M+0x119C and, when enabling,
// M+0x11A0..+0x11A8 (a leaf; game thread only). Entry: mov rax,[rip+...] (g_EndGameRoot); mov r8,[rax+180h];
// mov [r8+119Ch],cl. The first instruction's displacement is within the image. Disabling writes the zero vector,
// the resting state (RE-p2-charapop-two-centres.md §1).
constexpr std::uint8_t kSetCentreBytes[] = {0x48, 0x8B, 0x05, 0xE1, 0xBE, 0x84, 0x07, 0x4C,
                                            0x8B, 0x80, 0x80, 0x01, 0x00, 0x00, 0x41, 0x88};
constexpr platform::FunctionFact kSetCentre{L"CharaPop_SetOverrideCenter", 0x1418B2F00, kSetCentreBytes};
using SetCentreFn = void (*)(bool enable, const float* centre);

// CharaPopEntry_EvalSpawnScore_ByCenter(entry) -> int32 (1.005 0x1408AFC14; the same first 21 bytes): the distance part
// of an entry's spawn score, from the override centre when set, else the leader's. Its first instruction crosses a
// 16-byte block in 1.004, so the redirect sits at the second there (both are home-area stores; the second has no other
// xref, §1).
constexpr std::uint8_t kScoreBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10,  // mov [rsp+10h],rbx
                                        0x48, 0x89, 0x7C, 0x24, 0x18,  // mov [rsp+18h],rdi
                                        0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0xE0, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kScore{L"CharaPopEntry_EvalSpawnScore_ByCenter", 0x1408B5B8C, kScoreBytes};
constexpr std::size_t kFirstInstruction  = 5;
constexpr std::size_t kSecondInstruction = 5;
using ScoreFn = std::int32_t (*)(void* entry);

// The manager M (the entry's +0: its manager) as the score reads it, and the pop object's selection (§1, §5).
constexpr std::uint32_t  kOverrideOn     = 0x119C;  // u8
constexpr std::uint32_t  kOverrideCentre = 0x11A0;  // FVector
constexpr std::uint32_t  kLeaderOn       = 0x11AE;  // u8 (the score's own test)
constexpr std::uint32_t  kManagerSelect  = 0x18;    // pointer; the threshold at +0xE8 (i32)
constexpr std::uint32_t  kThreshold      = 0xE8;
constexpr std::uint32_t  kSelectionTask  = 0x28;    // the selection's FAsyncTask while it runs
constexpr std::uint32_t  kSpawnBudget    = 0x6170;  // P: i32, the spawn budget (128 live)
constexpr std::uint32_t  kVisibleBudget  = 0x6174;  // P: i32
constexpr std::uint32_t  kEntryManager   = 0x00;
constexpr std::uint32_t  kEntryScore     = 0x44;    // i32, the final score
constexpr std::uint32_t  kEntryOldSlots  = 0x80;    // the score copies its old transform into +0x80..+0x10F
constexpr std::size_t    kOldSlotsSize   = 0x90;
constexpr std::uint32_t  kEntrySpawnAt   = 0x60;    // the spawn translation the score last wrote (FVector)
constexpr std::uint32_t  kEntryFlags     = 0x118;   // u32: bit 0 externally managed (score 0)
constexpr std::uint32_t  kEntryHidden    = 0x1A2;   // u8: spawn hidden (over the visible budget)
constexpr std::int32_t   kExcluded       = -999999;

constexpr std::uint32_t kEvery        = 10;  // ticks between the player-2 location reads
constexpr auto          kSummaryEvery = std::chrono::seconds(10);
constexpr float         kRings[]      = {3000.0f, 6000.0f, 10000.0f};  // 30 / 60 / 100 m

std::uintptr_t       g_gameRoot  = 0;  // set once at install
SetCentreFn          g_setCentre = nullptr;
platform::EntryRedirect g_scoreEntry;
ScoreFn              g_score     = nullptr;
bool                 g_installed = false;
std::atomic<bool>    g_enabled{true};

// Game thread (the score runs only there; §3).
bool              g_active     = false;  // split shown, player 2 plays, switched on: the second call runs
float             g_two[3]     = {};     // player 2's member's location, read each kEvery ticks
bool              g_twoRead    = false;
bool              g_eventCentre = false;  // an event's override centre was in place at the last tick
std::uint32_t     g_ticks      = 0;
platform::Every g_summary{kSummaryEvery};

struct Counts
{
    unsigned calls = 0, second = 0, twoWon = 0, deferred = 0, managed = 0, noLeader = 0;
    long long ticks = 0;  // QPC ticks inside the second calls
};
Counts g_counts{};

/// The replacement: the game's own score, and while player 2 plays the same score with player 2's member as the
/// override centre; the nearer player's (higher) wins. An event's centre, a cleared leader centre or an externally
/// managed entry keep the game's single result.
std::int32_t ScoreTwo(void* entry) noexcept
{
    const std::int32_t one = g_score(entry);
    if (!g_active || !g_twoRead || entry == nullptr || !generation::OnGameThread())
    {
        return one;
    }
    ++g_counts.calls;
    const void* manager = ReadPointer(entry, kEntryManager);
    if (manager == nullptr)
    {
        return one;
    }
    if (ReadField<std::uint8_t>(manager, kOverrideOn) != 0)
    {
        ++g_counts.deferred;  // the mod never leaves its centre set: this one is an event's
        return one;
    }
    if (ReadField<std::uint8_t>(manager, kLeaderOn) == 0)
    {
        ++g_counts.noLeader;  // the game has no centre, so the mod invents none
        return one;
    }
    if ((ReadField<std::uint32_t>(entry, kEntryFlags) & 1u) != 0)
    {
        ++g_counts.managed;
        return one;
    }
    LARGE_INTEGER start{}, end{};
    QueryPerformanceCounter(&start);
    std::byte saved[kOldSlotsSize];
    std::memcpy(saved, static_cast<const std::byte*>(entry) + kEntryOldSlots, sizeof(saved));
    if (!platform::CallGuarded(g_setCentre, true, static_cast<const float*>(g_two)))
    {
        return one;
    }
    const std::int32_t two     = g_score(entry);
    const float        zero[3] = {};
    platform::CallGuarded(g_setCentre, false, static_cast<const float*>(zero));
    std::memcpy(static_cast<std::byte*>(entry) + kEntryOldSlots, saved, sizeof(saved));
    QueryPerformanceCounter(&end);
    ++g_counts.second;
    g_counts.ticks += end.QuadPart - start.QuadPart;
    if (two > one)
    {
        ++g_counts.twoWon;
    }
    return std::max(one, two);
}

bool CharacterAt(const void* character, float (&out)[3]) noexcept
{
    return character != nullptr && charapop::ActorLocation(const_cast<void*>(character), out);
}

/// Logs an event's centre appearing or clearing (Info: rare).
void WatchEventCentre(const std::byte* manager, const float (&one)[3]) noexcept
{
    const bool on = ReadField<std::uint8_t>(manager, kOverrideOn) != 0;
    if (on == g_eventCentre)
    {
        return;
    }
    g_eventCentre = on;
    if (!on)
    {
        platform::Log(L"npc centre: the event's centre cleared; each NPC is scored from the nearer player again");
        return;
    }
    float centre[3]{};
    platform::CopyGuarded(centre, manager + kOverrideCentre, sizeof(centre));
    platform::Log(L"npc centre: an event set its own centre ({:.0f}, {:.0f}, {:.0f}; {:.0f} m from player 1, {:.0f} m from "
                  L"player 2); the population follows it alone until it clears",
                  centre[0], centre[1], centre[2], charapop::Distance(centre, one) / 100.0f,
                  g_twoRead ? charapop::Distance(centre, g_two) / 100.0f : -1.0f);
}

/// The 10 s status line (Debug): the second calls, the budgets, and the spawned entries around each player.
void LogSummary(const charapop::Objects& objects, const float (&one)[3]) noexcept
{
    const auto now = Clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    const Counts c = std::exchange(g_counts, {});
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    std::int32_t     count = 0;
    const std::byte* list  = charapop::EntryList(objects.pop, count);
    int spawned = 0, hidden = 0, excluded = 0, nearOne[3] = {}, nearTwo[3] = {};
    for (std::int32_t n = 0; n < count; ++n)
    {
        const void* entry = ReadPointer(list, static_cast<std::uint32_t>(n) * sizeof(void*));
        if (entry == nullptr)
        {
            continue;
        }
        excluded += ReadField<std::int32_t>(entry, kEntryScore) == kExcluded ? 1 : 0;
        hidden   += ReadField<std::uint8_t>(entry, kEntryHidden) != 0 ? 1 : 0;
        const auto state = ReadField<std::int32_t>(entry, charapop::kEntryState);
        if (state < 4 || state > 8)
        {
            continue;
        }
        ++spawned;
        float at[3]{};
        if (!platform::CopyGuarded(at, static_cast<const std::byte*>(entry) + kEntrySpawnAt, sizeof(at)))
        {
            continue;
        }
        const float toOne = charapop::Distance(at, one);
        const float toTwo = g_twoRead ? charapop::Distance(at, g_two) : 1e30f;
        for (int r = 0; r < 3; ++r)
        {
            nearOne[r] += toOne < kRings[r] ? 1 : 0;
            nearTwo[r] += toTwo < kRings[r] ? 1 : 0;
        }
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double us     = frequency.QuadPart > 0 ? static_cast<double>(c.ticks) * 1e6 / frequency.QuadPart : 0.0;
    const void*  select = ReadPointer(objects.manager, kManagerSelect);
    platform::LogDebug(L"npc centre (10 s): scored from the nearer player: {} calls, {} second calls ({} won by player 2, "
                       L"{:.0f} us in all), the game's single score for an event's centre {}, no leader centre {}, "
                       L"managed entries {}; entries {}, spawned {} (budget {}, visible {}, threshold {}, selection task "
                       L"{}), spawn hidden {}, excluded {}; spawned within 30/60/100 m of player 1 {}/{}/{}, of player 2 "
                       L"{}/{}/{} ({:.0f} m apart)",
                       c.calls, c.second, c.twoWon, us, c.deferred, c.noLeader, c.managed, count, spawned,
                       ReadField<std::int32_t>(objects.pop, kSpawnBudget), ReadField<std::int32_t>(objects.pop, kVisibleBudget),
                       select != nullptr ? ReadField<std::int32_t>(select, kThreshold) : -1,
                       ReadPointer(objects.manager, kSelectionTask) != nullptr ? L"running" : L"idle", hidden, excluded,
                       nearOne[0], nearOne[1], nearOne[2], nearTwo[0], nearTwo[1], nearTwo[2],
                       g_twoRead ? charapop::Distance(one, g_two) / 100.0f : -1.0f);
}

void Pass() noexcept
{
    const charapop::Objects objects = charapop::ObjectsOf(g_gameRoot);
    if (objects.manager == nullptr)
    {
        g_twoRead = false;
        return;
    }
    float       one[3]{};
    const void* playerOne = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    const bool  oneRead   = CharacterAt(playerOne, one);
    g_twoRead             = CharacterAt(generation::Member(), g_two);
    WatchEventCentre(objects.manager, one);
    if (oneRead)
    {
        LogSummary(objects, one);
    }
}

void SetActive(bool active, const wchar_t* why) noexcept
{
    if (active == g_active)
    {
        return;
    }
    g_active = active;
    if (!active)
    {
        g_twoRead = false;
    }
    platform::Log(L"npc centre: {} ({})",
                  active ? L"each NPC is scored from the nearer player (the game's own score, once per player)"
                         : L"the game's own score from the leader",
                  why);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root   = image.Rebase(charapop::kGameRoot, sizeof(void*));
    const auto setter = platform::FindFunction(image, kSetCentre);
    const auto score  = platform::FindFunction(image, kScore);
    g_gameRoot        = root ? *root : 0;
    g_setCentre       = setter ? reinterpret_cast<SetCentreFn>(*setter) : nullptr;
    bool placed       = false;
    if (g_gameRoot != 0 && g_setCentre != nullptr && score)
    {
        const std::span<const std::uint8_t> bytes(kScoreBytes);
        const auto replacement = reinterpret_cast<std::uintptr_t>(&ScoreTwo);
        placed = g_scoreEntry.Install(*score, image.EntryInstruction(kScore.address, bytes.first(kFirstInstruction)),
                                      replacement) ||
                 g_scoreEntry.InstallAtSecondInstruction(*score, bytes.first(kFirstInstruction),
                                                         bytes.subspan(kFirstInstruction, kSecondInstruction), replacement);
        if (placed)
        {
            g_score = reinterpret_cast<ScoreFn>(g_scoreEntry.Original());
        }
    }
    g_installed = placed;
    platform::Log(L"npc centre: {}", g_installed ? L"installed (while split, each NPC is scored from the nearer player with "
                                                   L"the game's own score; Ctrl+F10 switches it for comparison)"
                                                 : L"UNAVAILABLE (a check failed); the population follows the leader alone");
    return g_installed;
}

void Tick(bool splitShown, bool playerTwoPlays) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    if (!splitShown || !playerTwoPlays || !generation::Active() || !g_enabled.load(std::memory_order_relaxed))
    {
        SetActive(false, !g_enabled.load(std::memory_order_relaxed) ? L"switched off"
                                                                    : L"the split closed or player 2 stopped playing");
        g_eventCentre = false;
        return;
    }
    SetActive(true, L"split, player 2 plays");
    if (++g_ticks % kEvery == 0 || !g_twoRead)
    {
        Pass();
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled.store(on, std::memory_order_relaxed);
    g_summary.Reset();
    platform::Log(L"npc centre: switched {} (the population {})", on ? L"on" : L"off",
                  on ? L"is scored from the nearer player while split" : L"follows the leader, as the game alone does");
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

void Release() noexcept
{
    if (!g_installed)
    {
        return;
    }
    // Nothing persistent is written (the second call clears its centre itself); only the redirect goes back. A call
    // already inside the replacement keeps using the trampoline.
    g_active = false;
    if (!g_scoreEntry.Restore())
    {
        platform::Log(L"npc centre: the score's entry was changed after us; left in place (it passes through)");
    }
}

bool Installed() noexcept
{
    return g_installed;
}

}  // namespace lss::player_two::charapop_centre
