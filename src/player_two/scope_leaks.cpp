#include "player_two/scope_leaks.h"

#include "facts/controller.h"
#include "player_two/field_rules.h"
#include "platform/byte_scope.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/wheelie_ride.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace lss::player_two::scope_leaks {

namespace {

// Leak 1 (RE-p2-scope-leak-audit.md, "Patch facts"): the component vt+0x428 block at 0x14125C189.
// Its `inc qword [rip+...]` at 0x14125C1A7 names the frame count; the check of those bytes is the
// check that the three globals are where the research read them.
constexpr std::uint8_t   kClockIncrementBytes[] = {0x48, 0xFF, 0x05, 0x52, 0x2C, 0xEA, 0x07};
constexpr platform::FunctionFact kClockIncrement{L"the clock's frame-count increment", 0x14125C1A7,
                                                 kClockIncrementBytes};
constexpr std::uintptr_t kClockFrames         = 0x1490FEE00;  // int64
constexpr std::uintptr_t kClockSeconds        = 0x1490FEE08;  // double: seconds += (double)dt
constexpr std::uintptr_t kClockMs             = 0x146FA9A00;  // int64: trunc(seconds * 1000.0)
constexpr std::uint8_t   kClockMsScaleBytes[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x8F, 0x40};
// the 1000.0 it multiplies by
constexpr platform::FunctionFact kClockMsScale{L"the clock's 1000.0 multiplier", 0x1460A2D88, kClockMsScaleBytes};
constexpr std::uint32_t  kComponentGuard      = 0x9A4;        // bit 0: this component advanced the clock this frame
constexpr int            kNudges              = 4;

// Leak 2: the leader block written by 0x14125C394 (A only): a dword, the 48-byte transform at
// +0x10, two 12-byte vectors and dt. The byte after it (0x14911F28C) is an unrelated flag.
constexpr std::uintptr_t kLeaderBlock     = 0x14911F230;
constexpr std::size_t    kLeaderBlockSize = 0x5C;

// Leak 5: slot 16's one call to MoveStateBase__FieldSummonBody (rcx = state, xmm1 = dt, unused).
constexpr std::uintptr_t kSummonCall  = 0x1407BC991;
constexpr std::uint8_t   kSummonBodyPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                                  0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xC0, 0x01,
                                                  0x00, 0x00, 0xE8, 0x8F, 0x39, 0x8F, 0x01, 0x48, 0x2B, 0xE0};
constexpr platform::FunctionFact kSummonBody{L"MoveStateBase__FieldSummonBody", 0x1407BB584, kSummonBodyPrologue};

using SummonBodyFn = void (*)(void* state, float deltaTime);

// Set once at install.
std::int64_t*          g_frames  = nullptr;
double*                g_seconds = nullptr;
std::int64_t*          g_ms      = nullptr;
std::byte*             g_leader  = nullptr;
platform::CallRedirect g_summonCall;

std::atomic<unsigned> g_mask{kAllFixes};

// Game thread.
void* g_memberState = nullptr;  // player 2's member's state, inside the mod's tick only

struct Counts
{
    unsigned clockAdvanced  = 0;  // the member's tick advanced the clock
    unsigned clockInexact   = 0;  // ...onto seconds other than player 1's (the ms stamp may be 1 ms off)
    unsigned clockPutBack   = 0;
    unsigned clockMissed    = 0;  // advanced although the fix is on and the guard said it would not
    unsigned leaderRewrites = 0;  // player 2's input changed the leader block
    unsigned leaderPutBack  = 0;
    unsigned summonSkips    = 0;
};
Counts g_counts;

bool Acts(unsigned fix) noexcept
{
    return (g_mask.load(std::memory_order_relaxed) & fix) != 0;
}

/// The fixes whose facts did not hold at install, for the status line ("" if none).
const wchar_t* Unavailable() noexcept;

bool InPlayerTwoScope() noexcept
{
    return generation::OnGameThread() && generation::Active();
}

void SummonBody(void* state, float deltaTime) noexcept
{
    if (state != nullptr && state == g_memberState && Acts(kFixSummonBody) && generation::OnGameThread())
    {
        ++g_counts.summonSkips;
        // Its key-4 part, the Wheelie walk-in question, asked for player 2 instead (player_two/wheelie_ride.h).
        wheelie_ride::Ask(state);
        return;  // as for any character the game does not tick
    }
    reinterpret_cast<SummonBodyFn>(g_summonCall.Original())(state, deltaTime);
}

/// The seconds from which adding dt the game's way lands exactly on `target`, if one is near.
double SecondsBefore(double target, float deltaTime) noexcept
{
    return field_rules::SecondsBefore(target, deltaTime, kNudges);
}

// Saved between a scope's construction and destruction (neither kind nests).
std::int64_t                              g_savedFrames  = 0;
double                                    g_savedSeconds = 0.0;
std::int64_t                              g_savedMs      = 0;
bool                                      g_rewound      = false;
bool                                      g_inputOpen    = false;  // an InputScope holds the saved block
bool                                      g_tickOpen     = false;  // a MemberTickScope holds the saved clock
// The leader block around player 2's input: saved and put back whatever the input wrote (defined at install).
platform::ByteStore<1, kLeaderBlockSize>  g_leaderBlock;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto frames  = image.Rebase(kClockFrames, sizeof(std::int64_t));
    const auto seconds = image.Rebase(kClockSeconds, sizeof(double));
    const auto ms      = image.Rebase(kClockMs, sizeof(std::int64_t));
    if (frames && seconds && ms && platform::FindFunction(image, kClockIncrement) &&
        platform::FindFunction(image, kClockMsScale))
    {
        g_frames  = reinterpret_cast<std::int64_t*>(*frames);
        g_seconds = reinterpret_cast<double*>(*seconds);
        g_ms      = reinterpret_cast<std::int64_t*>(*ms);
    }
    if (const auto leader = image.Rebase(kLeaderBlock, kLeaderBlockSize))
    {
        const platform::ByteContext::Range block{*leader, kLeaderBlockSize, platform::ByteContext::Kind::Keep};
        if (g_leaderBlock.Define(std::span<const platform::ByteContext::Range>(&block, 1)))
        {
            g_leader = reinterpret_cast<std::byte*>(*leader);
        }
    }
    const auto site   = image.Rebase(kSummonCall, 5);
    const auto target = platform::FindFunction(image, kSummonBody);
    const bool summon = site && target &&
                        g_summonCall.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&SummonBody));
    platform::Log(L"scope leaks: clock {}, leader transform {}, field summons {}",
                  g_frames != nullptr ? L"put back" : L"UNAVAILABLE (bytes differ)",
                  g_leader != nullptr ? L"put back" : L"UNAVAILABLE", summon ? L"skipped for player 2" : L"UNAVAILABLE");
    return g_frames != nullptr || g_leader != nullptr || summon;
}

void Remove() noexcept
{
    if (g_summonCall.IsInstalled() && !g_summonCall.Restore())
    {
        platform::Log(L"scope leaks: the summon call was changed after us; left in place (it passes other states through)");
    }
}

void SetMask(unsigned mask) noexcept
{
    g_mask.store(mask, std::memory_order_relaxed);
    platform::Log(L"scope leaks: mask {} (clock {}, leader transform {}, field summons {})", mask, Acts(kFixClock),
                  Acts(kFixLeaderTransform), Acts(kFixSummonBody));
}

InputScope::InputScope(bool playerTwo) noexcept
{
    // With the fix off (diagnosis) the block is left as player 2's input writes it, as without this scope.
    if (!playerTwo || g_leader == nullptr || g_inputOpen || !InPlayerTwoScope() || !Acts(kFixLeaderTransform) ||
        !g_leaderBlock.Open())
    {
        return;  // (an open scope already holds the block: this one leaves it to it)
    }
    g_inputOpen = true;
    held_       = true;
}

InputScope::~InputScope()
{
    if (!held_)
    {
        return;
    }
    g_inputOpen = false;
    const unsigned rewrote = g_leaderBlock.Close();  // the block as it was found, whatever the input wrote
    g_counts.leaderRewrites += rewrote;
    g_counts.leaderPutBack += rewrote;
}

MemberTickScope::MemberTickScope(void* state, const void* playerTwoController, float deltaTime) noexcept
{
    if (g_tickOpen || !InPlayerTwoScope())
    {
        return;
    }
    g_memberState = state;
    const void* component = ReadPointer(playerTwoController, facts::kControllerLeaderActions);
    if (g_frames == nullptr || component == nullptr)
    {
        return;
    }
    g_savedFrames  = *g_frames;
    g_savedSeconds = *g_seconds;
    g_savedMs      = *g_ms;
    clockHeld_     = true;
    g_tickOpen     = true;
    // Set back one step only when the tick will advance it: player 2's component has not yet this frame.
    g_rewound = Acts(kFixClock) && (ReadField<std::uint8_t>(component, kComponentGuard) & 1) == 0;
    if (g_rewound)
    {
        *g_frames  = g_savedFrames - 1;
        *g_seconds = SecondsBefore(g_savedSeconds, deltaTime);
    }
}

MemberTickScope::~MemberTickScope()
{
    if (!clockHeld_)
    {
        if (!g_tickOpen)
        {
            g_memberState = nullptr;
        }
        return;
    }
    g_memberState = nullptr;
    g_tickOpen    = false;
    const bool rewound = g_rewound;
    const bool advanced = *g_frames != (rewound ? g_savedFrames - 1 : g_savedFrames);
    if (!advanced)
    {
        if (rewound)
        {
            *g_frames  = g_savedFrames;  // the tick did not advance it: back as it was
            *g_seconds = g_savedSeconds;
            *g_ms      = g_savedMs;
        }
        return;
    }
    ++g_counts.clockAdvanced;
    if (!rewound)
    {
        // With the fix off (diagnosis) the second advance stays, as without this scope. With it on, the
        // guard bit said the tick would not advance the clock, and it did: counted, and put back.
        if (!Acts(kFixClock))
        {
            return;
        }
        ++g_counts.clockMissed;
        *g_frames  = g_savedFrames;
        *g_seconds = g_savedSeconds;
        *g_ms      = g_savedMs;
        return;
    }
    g_counts.clockInexact += *g_seconds != g_savedSeconds ? 1 : 0;
    *g_frames  = g_savedFrames;
    *g_seconds = g_savedSeconds;
    *g_ms      = g_savedMs;
    ++g_counts.clockPutBack;
}

namespace {

const wchar_t* Unavailable() noexcept
{
    const bool clock  = g_frames == nullptr;
    const bool leader = g_leader == nullptr;
    const bool summon = !g_summonCall.IsInstalled();
    if (!clock && !leader && !summon)
    {
        return L"";
    }
    return clock && leader && summon ? L"; NONE AVAILABLE, all three leaks live"
           : clock                   ? L"; the clock fix is UNAVAILABLE"
           : leader                  ? L"; the leader-transform fix is UNAVAILABLE"
                                     : L"; the summon skip is UNAVAILABLE";
}

}  // namespace

void LogStatus() noexcept
{
    const Counts counts = g_counts;
    g_counts            = {};
    platform::Log(L"  scope leaks (mask {}{}): player 2's member's tick advanced the clock {} time(s), put back {} ({} "
                  L"inexact, {} advanced past the guard); player 2's input rewrote the leader transform {} time(s), put "
                  L"back {}; field summons skipped {} time(s)",
                  g_mask.load(std::memory_order_relaxed), Unavailable(), counts.clockAdvanced, counts.clockPutBack,
                  counts.clockInexact, counts.clockMissed, counts.leaderRewrites, counts.leaderPutBack, counts.summonSkips);
}

}  // namespace lss::player_two::scope_leaks
