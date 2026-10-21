#include "player_two/nav_strand.h"

#include "dev/dev_switch.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/call_site.h"
#include "player_two/chocobo_ride.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/party_warp.h"
#include "player_two/strand_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::player_two::nav_strand {

namespace {

using RC::Unreal::UObject;

// RE-p2-party-leash.md §2.1 (this build). The call is the one after the navmesh push-back found no
// navmesh; the game zeroes the move delta when it returns false.
constexpr std::uint8_t kReadyAtBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
                                          0x56, 0x41, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xCF, 0xF8, 0x67};
constexpr platform::FunctionFact kReadyAt{L"EndWorld_IsReadyAt_NavCellAndGrids", 0x140A2F648, kReadyAtBytes};
constexpr std::uintptr_t kReadyCall = 0x140A538C6;  // in EndCMC_ConstrainMoveDelta_NavMeshOnly

// The caller's frame around the call: `lea rdx, [rsp+0B8h]` (the location it asks about, the updated
// component's world location), and after the call, on false, `vmovups [rsp+90h], xmm0` storing zero
// into the move delta. So the move being refused lies 0x28 bytes below the location passed.
constexpr std::uintptr_t kLocationArgument  = 0x140A53846;
constexpr std::uint8_t   kLocationArgumentBytes[] = {0x48, 0x8D, 0x94, 0x24, 0xB8, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kAfterCall         = 0x140A538CB;
constexpr std::uint8_t   kAfterCallBytes[] = {0x84, 0xC0, 0x75, 0x2D, 0xC5, 0xFA, 0x11, 0xB4, 0x24, 0x80, 0x00, 0x00,
                                              0x00, 0xC5, 0xFA, 0x11, 0xB4, 0x24, 0x84, 0x00, 0x00, 0x00, 0xC5, 0xFA,
                                              0x11, 0xB4, 0x24, 0x88, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x10, 0x84, 0x24,
                                              0x80, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x11, 0x84, 0x24, 0x90, 0x00, 0x00,
                                              0x00};
constexpr std::ptrdiff_t kMoveBelowLocation = 0xB8 - 0x90;

// The falling step's call (RE-p2-party-leash.md §6c): EndCMC_NewFallVelocity_WorldReadyFreeze asks the same
// question with the same arguments when CMC+0xB6B is set (`cmp byte [rcx+0B6Bh], 0` before it; `lea rdx,
// [rbp-51h]`, a copy of the updated component's world location) and on false returns a zero velocity.
constexpr std::uintptr_t kFallCall          = 0x140F22706;
constexpr std::uintptr_t kFallGate          = 0x140F22659;
constexpr std::uint8_t   kFallGateBytes[]   = {0x80, 0xB9, 0x6B, 0x0B, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kFallLocation      = 0x140F226BD;
constexpr std::uint8_t   kFallLocationBytes[] = {0x48, 0x8D, 0x55, 0xAF};
constexpr std::uintptr_t kFallAfterCall     = 0x140F2270B;
constexpr std::uint8_t   kFallAfterCallBytes[] = {0x84, 0xC0, 0x75, 0x13, 0x48, 0xC7, 0x03, 0x00, 0x00,
                                                  0x00, 0x00, 0xC7, 0x43, 0x08, 0x00, 0x00, 0x00, 0x00};
// 1.005 (RE-1005-field-2.md §2): the falling step's call is at 0x140C5FAEE, %16 = 14, so it crosses a 16-byte block,
// and characters move on worker threads too, so no one thread can change it (platform/call_redirect.h). There the
// falls are heard at the entry of EndWorld_IsReadyAt instead (its first instruction `mov [rsp+10h], rdx`, 5 bytes at
// %16 = 8): the replacement counts only the answers that return to the falling step's call and passes every other
// caller through unchanged.
constexpr std::size_t kReadyAtFirstInstruction = 5;

// The safe-transform check (RE-p2-party-leash.md §6d): every 0.12 s EndChar_UpdateSafeTransform_Check asks the same
// question about the character's root with the nav clause off (`xor r8d, r8d`; `lea rdx, [rsp+50h]`, a copy of the
// root's world location; the reason byte zeroed and passed in rcx) and on false marks the spot unsafe (`S+0x70 = 1`).
// The next pawn tick either return-warps her to her last safe spot or, standing on it, parks her: stop reason 5 in
// her movement owner's stop mask, which forces MOVE_None, so neither call above is asked any more.
constexpr std::uintptr_t kParkCall            = 0x140DECB06;
constexpr std::uintptr_t kParkNavFlag         = 0x140DECAE9;
constexpr std::uint8_t   kParkNavFlagBytes[]  = {0x45, 0x33, 0xC0};
constexpr std::uintptr_t kParkLocation        = 0x140DECAF1;
constexpr std::uint8_t   kParkLocationBytes[] = {0x48, 0x8D, 0x54, 0x24, 0x50, 0xC6, 0x44, 0x24, 0x40,
                                                 0x00, 0x48, 0x8D, 0x4C, 0x24, 0x40};
constexpr std::uintptr_t kParkAfterCall       = 0x140DECB0B;
constexpr std::uint8_t   kParkAfterCallBytes[] = {0x84, 0xC0, 0x75, 0x09, 0xC6, 0x43, 0x70,
                                                  0x01, 0xE9, 0xF1, 0x02, 0x00, 0x00};
// The character's movement owner (a native SE object) and its 32-bit stop-reason mask; reason 5 is the
// safe-transform park (read live: 0x20 on the parked member, 0 on player 1).
// 1.005: +0x1418 (End character native field +0x20): the char interface (pawn+0x4B8) accessor `cmp qword
// [rcx+0F40h],0` (1.004 0x140889748) is `cmp qword [rcx+0F60h],0` in 1.005 (0x1408B3A04), unique in both builds.
constexpr platform::PerBuild<std::uint32_t> kCharacterMoveOwner = {0x13F8, 0x1418};
constexpr std::uint32_t kOwnerStopMask      = 0x1BC;
constexpr std::uint32_t kStopReasonParked   = 1u << 5;

// USceneComponent's world transform translation (the location the call site copies, +0x1D0).
constexpr std::uint32_t kComponentWorldLocation = 0x1D0;
// USceneComponent::AttachParent (UE4SS's header dump; read null live on a member walking on foot).
using facts::kSceneComponentAttachParent;

// The nav streaming manager (a native singleton) and its current (leader) cell, three floats.
constexpr std::uintptr_t kNavStreamManagerGlobal = 0x14939BCD8;
constexpr std::uint32_t  kNavStreamLeaderCell    = 0x8;

// The retained AI's current state (RE-p2-party-warp.md §1 and its sampler table).
constexpr std::uint32_t  kAiBrain            = 0x518;
constexpr std::uint32_t  kAiPawn             = 0x258;
constexpr std::uint32_t  kAiPauseMask        = 0x460;
constexpr std::uint32_t  kBrainState         = 0x20;
constexpr std::uint32_t  kStateType          = 0x18;
constexpr std::uint32_t  kStateTeleportRange = 0x344;
constexpr std::uint32_t  kStateTeleporting   = 0x39D;
constexpr std::uint8_t   kStateFollow        = 7;
using facts::kFollowStatePlayerVTable;

constexpr float kMaxTickSeconds = 0.25f;  // a hitch counts as this much, so one frame never completes a hold

using ReadyAtFn = bool (*)(void* reason, const float* location, bool flag);

// Set once at install.
platform::CallRedirect g_readyCall;
platform::CallRedirect g_fallCall;
platform::CallRedirect g_parkCall;
platform::EntryRedirect g_readyEntry;           // 1.005: the falls, heard at the entry
std::uintptr_t         g_fallReturn         = 0;  // 1.005: live return address of the falling step's call
std::uintptr_t        g_navStreamManager   = 0;  // live address of the global
std::uintptr_t         g_followStateVTable  = 0;
bool                   g_installed          = false;

// Written on the game thread, read by the redirect on whichever thread moves the member (the game
// moves characters on worker threads too, so the redirect counts from any thread).
std::atomic<const std::byte*> g_memberRoot{nullptr};
// Per tick (taken by Tick, dropped when the member changes):
std::atomic<std::uint32_t> g_tickRefusals{0};        // refused moves since the last tick
std::atomic<float>         g_tickLongestMove{0.0f};  // the longest refused move since the last tick, cm
std::atomic<std::uint32_t> g_tickFallRefusals{0};    // of those, refused falls (she hangs in the air)
std::atomic<std::uint32_t> g_tickUnsafeChecks{0};    // safe-transform checks that found her spot not ready
// The chocobo player 2 rides (RE-p2-chocobo-warp.md §1): its moves go through the same three calls. Taken each tick
// by MountedTick (its own strand watch; the warp is the chocobo's follow warp).
std::atomic<const std::byte*> g_chocoboRoot{nullptr};
std::atomic<std::uint32_t> g_chocoboRefusals{0};
std::atomic<std::uint32_t> g_chocoboFalls{0};
std::atomic<std::uint32_t> g_chocoboUnsafe{0};
// For the whole session (logged as they grow):
std::atomic<std::uint32_t> g_readyNoNavmesh{0};  // moves that found no navmesh where the world was ready (not refused)
std::atomic<std::uint32_t> g_offThread{0};       // answers for the member that came off the game thread

struct Strand
{
    std::uint32_t refusals    = 0;
    std::uint32_t falls       = 0;  // of the refusals, refused falls
    std::uint32_t parkChecks  = 0;  // failed safe-transform checks while the game held her parked
    float       longestMove = 0.0f;
    float         start[3]{};
    bool          started     = false;  // `start` was read
};

// The hold is shorter than the game's 1.0 s for AI followers: a refusal already means the navmesh under
// her is not loaded, and a player running in place should not wait; 0.3 s only rides out a cell that is
// still loading. The cooldown is the game's; the grace is StrandWatch's default (spans refused-fall bursts).
constexpr float kHoldSeconds = 0.3f;
StrandWatch     g_watch{StrandRules{.holdSeconds = kHoldSeconds}};
Strand      g_strand;
void*       g_member       = nullptr;
std::int32_t g_rootOffset  = -1;
std::chrono::steady_clock::time_point g_lastTick{};
bool          g_ticked      = false;
bool          g_loggedOffThread = false;
std::uint32_t g_loggedReadyNoNavmesh = 0;
#if LSS_DEV
const void*   g_loggedState = nullptr;  // the AI probe's last state (dev tools)
std::uint8_t  g_loggedType  = 0xFF;
#endif

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // a Debug report (platform/log.h)
    }
    try
    {
        platform::WriteLine(L"[debug] nav strand: " + std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
}

const std::byte* At(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? nullptr : static_cast<const std::byte*>(object) + offset;
}

/// True when `location` is player 2's member's. The call sites pass no component, only a copy of its
/// world location, so the member is recognised by an exact match with her root's; another character
/// would have to stand on the very same floats.
bool IsAt(const std::atomic<const std::byte*>& rootOf, const float* location) noexcept
{
    const std::byte* root = rootOf.load(std::memory_order_acquire);
    float            at[3]{};
    return root != nullptr && platform::CopyGuarded(at, root + kComponentWorldLocation, sizeof(at)) &&
           std::memcmp(at, location, sizeof(at)) == 0;
}

bool IsMember(const float* location) noexcept
{
    return IsAt(g_memberRoot, location);
}

/// Counts a "not ready" answer for the chocobo player 2 rides in `counter` (refused moves, refused falls, or failed
/// safe-transform checks).
void CountChocobo(const float* location, bool ready, std::atomic<std::uint32_t>& counter) noexcept
{
    if (!ready && IsAt(g_chocoboRoot, location))
    {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteThread() noexcept
{
    if (!generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Counts one of the member's answers; true if it was a refusal.
bool Count(bool ready) noexcept
{
    NoteThread();
    if (ready)
    {
        g_readyNoNavmesh.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_tickRefusals.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/// The walking redirect: the game's answer, unchanged; a refusal zeroes the member's move.
bool ReadyAt(void* reason, const float* location, bool flag) noexcept
{
    const bool ready = reinterpret_cast<ReadyAtFn>(g_readyCall.Original())(reason, location, flag);
    CountChocobo(location, ready, g_chocoboRefusals);
    if (!IsMember(location) || !Count(ready))
    {
        return ready;
    }
    float move[3]{};
    if (platform::CopyGuarded(move, reinterpret_cast<const std::byte*>(location) - kMoveBelowLocation, sizeof(move)))
    {
        const float length = std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
        float       longest = g_tickLongestMove.load(std::memory_order_relaxed);
        while (length > longest &&
               !g_tickLongestMove.compare_exchange_weak(longest, length, std::memory_order_relaxed))
        {
        }
    }
    return ready;
}

/// The falling redirect: the game's answer, unchanged; a refusal zeroes the member's fall velocity, so she
/// hangs in the air, every tick, stick or no stick.
void CountFall(const float* location, bool ready) noexcept
{
    CountChocobo(location, ready, g_chocoboFalls);
    if (IsMember(location) && Count(ready))
    {
        g_tickFallRefusals.fetch_add(1, std::memory_order_relaxed);
    }
}

bool FallReadyAt(void* reason, const float* location, bool flag) noexcept
{
    const bool ready = reinterpret_cast<ReadyAtFn>(g_fallCall.Original())(reason, location, flag);
    CountFall(location, ready);
    return ready;
}

/// 1.005: every call of the function, at its entry. Only the falling step's (by its return address) is counted;
/// the walking and safe-transform calls reach here from their own redirects above (a return address in this DLL)
/// and every other caller passes through.
bool EntryReadyAt(void* reason, const float* location, bool flag) noexcept
{
    const bool fall  = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_fallReturn;
    const bool ready = reinterpret_cast<ReadyAtFn>(g_readyEntry.Original())(reason, location, flag);
    if (fall)
    {
        CountFall(location, ready);
    }
    return ready;
}

/// The safe-transform redirect: the game's answer, unchanged. Only counted; whether it strands her is the
/// game's own park bit (Parked), read on the game thread.
bool SafeCheckReadyAt(void* reason, const float* location, bool flag) noexcept
{
    const bool ready = reinterpret_cast<ReadyAtFn>(g_parkCall.Original())(reason, location, flag);
    CountChocobo(location, ready, g_chocoboUnsafe);
    if (!ready && IsMember(location))
    {
        NoteThread();
        g_tickUnsafeChecks.fetch_add(1, std::memory_order_relaxed);
    }
    return ready;
}

/// The game holds the member parked (stop reason 5, MOVE_None) until a safe-transform check passes.
bool Parked(const void* member) noexcept
{
    std::uint32_t mask = 0;
    return Read(At(ReadPointer(member, kCharacterMoveOwner), kOwnerStopMask), mask) && (mask & kStopReasonParked) != 0;
}

/// The member's root component, through the reflected RootComponent offset (found once).
const std::byte* RootOf(void* member) noexcept
{
    if (member == nullptr)
    {
        return nullptr;
    }
    if (g_rootOffset < 0)
    {
        try
        {
            auto* property = static_cast<UObject*>(member)->GetPropertyByNameInChain(STR("RootComponent"));
            if (property == nullptr || property->GetSize() != sizeof(void*))
            {
                return nullptr;
            }
            g_rootOffset = property->GetOffset_Internal();
        }
        catch (...)
        {
            return nullptr;
        }
    }
    return static_cast<const std::byte*>(ReadPointer(member, static_cast<std::uint32_t>(g_rootOffset)));
}

bool LocationOf(const void* pawn, float (&out)[3]) noexcept
{
    const std::byte* root = pawn == nullptr ? nullptr : RootOf(const_cast<void*>(pawn));
    return root != nullptr && platform::CopyGuarded(out, root + kComponentWorldLocation, sizeof(out));
}

std::wstring PlacesText()
{
    float one[3]{}, two[3]{}, cell[3]{};
    const void* leader = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    const bool  hasOne = LocationOf(leader, one);
    const bool  hasTwo = LocationOf(g_member, two);
    const void* manager = nullptr;
    const bool  hasCell = Read(reinterpret_cast<const void*>(g_navStreamManager), manager) &&
                         Read(At(manager, kNavStreamLeaderCell), cell);
    std::wstring text = hasTwo ? std::format(L"P2 ({:.0f}, {:.0f}, {:.0f})", two[0], two[1], two[2]) : L"P2 ?";
    text += hasOne ? std::format(L", P1 ({:.0f}, {:.0f}, {:.0f})", one[0], one[1], one[2]) : L", P1 ?";
    if (hasOne && hasTwo)
    {
        text += std::format(L", apart {:.0f} m", std::hypot(two[0] - one[0], two[1] - one[1]) / 100.0f);
    }
    text += hasCell ? std::format(L", leader nav cell ({:.0f}, {:.0f}, {:.0f})", cell[0], cell[1], cell[2])
                    : L", leader nav cell ?";
    return text;
}

/// Both players' positions, their distance and the leader's nav cell, for a log line.
std::wstring Places() noexcept
{
    try
    {
        return PlacesText();
    }
    catch (...)
    {
        return {};
    }
}

float MovedSinceStart() noexcept
{
    float now[3]{};
    return g_strand.started && LocationOf(g_member, now)
               ? std::hypot(now[0] - g_strand.start[0], now[1] - g_strand.start[1])
               : -1.0f;
}

#if LSS_DEV
/// The retained AI's current state, logged when it changes (and with `always`). Debug: nothing is read below it.
void ProbeAi(const wchar_t* when, bool always) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const void*   ai   = ReadPointer(g_member, facts::kCharacterRetainedAi);
    const void*   brain = ReadPointer(ai, kAiBrain);
    const void*   state = ReadPointer(brain, kBrainState);
    std::uint8_t  type  = 0xFF;
    Read(At(state, kStateType), type);
    if (!always && state == g_loggedState && type == g_loggedType)
    {
        return;
    }
    g_loggedState = state;
    g_loggedType  = type;
    std::uintptr_t vtable = 0;
    float          range  = 0.0f;
    std::uint8_t   warping = 0;
    std::uint32_t  pause  = 0;
    Read(state, vtable);
    Read(At(state, kStateTeleportRange), range);
    Read(At(state, kStateTeleporting), warping);
    Read(At(ai, kAiPauseMask), pause);
    const void* aiPawn = ReadPointer(ai, kAiPawn);
    const bool follow = type == kStateFollow;
    Line(L"{}: retained AI {} (pawn {}, pause mask {:#x}), state {} type {}{}; player follow vtable {}, teleport range "
         L"{:.0f}, warping {}",
         when, ai, aiPawn, pause, state, type, follow ? L" (FOLLOW)" : L"", vtable == g_followStateVTable ? L"yes" : L"no",
         follow ? range : 0.0f, follow ? warping : std::uint8_t{0});
}
#else
void ProbeAi(const wchar_t*, bool) noexcept {}  // the AI probe is a dev tool
#endif

std::chrono::steady_clock::time_point g_lastMounted{};
bool                                  g_mountedQuiet = true;  // the last mounted line had nothing to report
// The chocobo player 2 rides, stranded (RE-p2-chocobo-warp.md §4): its own watch, as the member's on foot.
StrandWatch   g_rideWatch{StrandRules{.holdSeconds = kHoldSeconds}};
Strand        g_rideStrand;
const void*   g_rideChocobo = nullptr;
// The counts since the last mounted line (game thread).
std::uint32_t g_secondRefusals = 0;
std::uint32_t g_secondFalls    = 0;
std::uint32_t g_secondUnsafe   = 0;

float ChocoboMovedSinceStart(const void* chocobo) noexcept
{
    float now[3]{};
    return g_rideStrand.started && LocationOf(chocobo, now)
               ? std::hypot(now[0] - g_rideStrand.start[0], now[1] - g_rideStrand.start[1])
               : -1.0f;
}

std::wstring ChocoboAt(const void* chocobo)
{
    float at[3]{};
    return LocationOf(chocobo, at) ? std::format(L"({:.0f}, {:.0f}, {:.0f})", at[0], at[1], at[2]) : std::wstring(L"?");
}

void EndRideStrand(const void* chocobo, const wchar_t* why) noexcept
{
    try
    {
        // A strand's start, warp and end are rare, real state changes: Info.
        platform::Log(L"nav strand: mounted strand ended ({}) after {:.2f} s: {} refused move(s), {} refused fall(s), {} "
                      L"failed check(s) while parked; the chocobo moved {:.0f} cm since it began, now at {}, parked {}; {}",
                      why, g_rideWatch.Held(), g_rideStrand.refusals, g_rideStrand.falls, g_rideStrand.parkChecks,
                      ChocoboMovedSinceStart(chocobo), ChocoboAt(chocobo), Parked(chocobo) ? L"yes" : L"no", Places());
    }
    catch (...)
    {
    }
}

/// While player 2 rides seated (`ridden`): the game's refusals of their chocobo's moves and falls and its failed safe
/// checks while the game holds it parked make a strand; held kHoldSeconds, the chocobo's own follow warp brings it
/// back with player 2 seated (party_warp, Subject::Chocobo). Once a second while anything is nonzero, a "mounted:"
/// line with the counts and the chocobo AI's state. `chocobo`: also while getting off (counted, never warped).
void MountedTick(void* member, const void* chocobo, const void* ridden, bool splitShown, float seconds) noexcept
{
    g_chocoboRoot.store(chocobo == nullptr ? nullptr : RootOf(const_cast<void*>(chocobo)), std::memory_order_release);
    const std::uint32_t refusals = platform::Take(g_chocoboRefusals);
    const std::uint32_t falls    = platform::Take(g_chocoboFalls);
    const std::uint32_t unsafe   = platform::Take(g_chocoboUnsafe);
    if (ridden != g_rideChocobo)
    {
        if (g_rideWatch.Stranded())
        {
            EndRideStrand(g_rideChocobo, ridden == nullptr ? L"player 2 is no longer seated on it" : L"another chocobo");
        }
        g_rideWatch.Reset();
        g_rideStrand  = {};
        g_rideChocobo = ridden;
    }
    if (chocobo == nullptr)
    {
        g_secondRefusals = g_secondFalls = g_secondUnsafe = 0;
        return;
    }
    g_secondRefusals += refusals;
    g_secondFalls += falls;
    g_secondUnsafe += unsafe;
    if (ridden != nullptr)
    {
        // As on foot: a failed check alone may still end in the game's own return-warp to the safe spot; it strands the
        // chocobo only once the game has parked it.
        const std::uint32_t parkChecks = unsafe > 0 && Parked(ridden) ? unsafe : 0;
        switch (g_rideWatch.Step(refusals > 0 || parkChecks > 0, seconds))
        {
            case StrandEvent::Began:
                g_rideStrand         = {};
                g_rideStrand.started = LocationOf(ridden, g_rideStrand.start);
                try
                {
                    platform::Log(L"nav strand: mounted strand began: the game {} player 2's chocobo at {} (the world "
                                  L"under it is not loaded); {}",
                                  refusals == 0 ? L"parked" : falls > 0 ? L"refused the fall of" : L"refused the move of",
                                  ChocoboAt(ridden), Places());
                }
                catch (...)
                {
                }
                break;
            case StrandEvent::Warp:
                platform::Log(L"nav strand: mounted strand held {:.2f} s: {} refused move(s), {} refused fall(s), {} "
                              L"failed check(s) while parked; warping the chocobo with player 2 seated",
                              g_rideWatch.Held(), g_rideStrand.refusals + refusals, g_rideStrand.falls + falls,
                              g_rideStrand.parkChecks + parkChecks);
                party_warp::Start(const_cast<void*>(ridden), party_warp::Subject::Chocobo, false, splitShown);  // logs its outcome
                break;
            case StrandEvent::Ended:
                EndRideStrand(ridden, L"no refusal for a moment: stick let go, the world is back, or it was brought back");
                break;
            case StrandEvent::None: break;
        }
        if (g_rideWatch.Stranded())
        {
            g_rideStrand.refusals += refusals;
            g_rideStrand.falls += falls;
            g_rideStrand.parkChecks += parkChecks;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastMounted < std::chrono::seconds(1))
    {
        return;
    }
    g_lastMounted = now;
    const std::uint32_t secondRefusals = std::exchange(g_secondRefusals, 0);
    const std::uint32_t secondFalls    = std::exchange(g_secondFalls, 0);
    const std::uint32_t secondUnsafe   = std::exchange(g_secondUnsafe, 0);
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_mountedQuiet = true;
        return;  // the "mounted:" line is a periodic status line (Debug): nothing is read for it below that level
    }
    const bool          parked         = Parked(chocobo);
    const bool          quiet          = secondRefusals == 0 && secondFalls == 0 && secondUnsafe == 0 && !parked;
    if (quiet && g_mountedQuiet)
    {
        return;
    }
    g_mountedQuiet      = quiet;
    const void*  ai     = ReadPointer(chocobo, facts::kCharacterRetainedAi);
    const void*  state  = ReadPointer(ReadPointer(ai, kAiBrain), kBrainState);
    std::uint8_t type   = 0xFF, warping = 0;
    std::uint32_t pause = 0;
    float        range  = 0.0f;
    Read(At(state, kStateType), type);
    Read(At(state, kStateTeleporting), warping);
    Read(At(state, kStateTeleportRange), range);
    Read(At(ai, kAiPauseMask), pause);
    float at[3]{};
    const bool read = LocationOf(chocobo, at);
    try
    {
        Line(L"mounted: player 2's chocobo in the last second: {} refused move(s), {} refused fall(s), {} failed safe "
             L"check(s); parked {}; its AI pause mask {:#x}, state type {}, teleport range {:.0f}, warping {}; chocobo at {}; {}",
             secondRefusals, secondFalls, secondUnsafe, parked ? L"yes" : L"no", pause, type, range, warping,
             read ? std::format(L"({:.0f}, {:.0f}, {:.0f})", at[0], at[1], at[2]) : std::wstring(L"?"), Places());
    }
    catch (...)
    {
    }
}

void EndStrand(const wchar_t* why) noexcept
{
    platform::Log(L"nav strand: ended ({}) after {:.2f} s: {} refused move(s) ({} of them falls), {} failed check(s) "
                  L"while parked, longest {:.1f} cm; moved {:.0f} cm since it began; parked now {}; {}",
                  why, g_watch.Held(), g_strand.refusals, g_strand.falls, g_strand.parkChecks, g_strand.longestMove,
                  MovedSinceStart(), Parked(g_member) ? L"yes" : L"no", Places());
}

void Forget() noexcept
{
    if (g_watch.Stranded())
    {
        // The last tick's counts, not yet taken, belong to the strand being closed.
        g_strand.refusals += platform::Take(g_tickRefusals);
        g_strand.falls += platform::Take(g_tickFallRefusals);
        const std::uint32_t unsafe = platform::Take(g_tickUnsafeChecks);
        g_strand.parkChecks += unsafe > 0 && Parked(g_member) ? unsafe : 0;
        g_strand.longestMove =
            std::max(g_strand.longestMove, g_tickLongestMove.exchange(0.0f, std::memory_order_relaxed));
        EndStrand(L"player 2's member changed or left");
    }
    g_watch.Reset();
    g_strand = {};
    g_member = nullptr;
    g_memberRoot.store(nullptr, std::memory_order_release);
    g_tickRefusals.store(0, std::memory_order_relaxed);  // the outgoing member's refusals, not yet taken by a tick
    g_tickLongestMove.store(0.0f, std::memory_order_relaxed);
    g_tickFallRefusals.store(0, std::memory_order_relaxed);
    g_tickUnsafeChecks.store(0, std::memory_order_relaxed);
#if LSS_DEV
    g_loggedState = nullptr;
    g_loggedType  = 0xFF;
#endif
    g_ticked      = false;
    g_rideWatch.Reset();
    g_rideStrand  = {};
    g_rideChocobo = nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto readyAt = platform::FindFunction(image, kReadyAt);
    const auto site    = image.Rebase(kReadyCall, 5);
    const auto manager = image.Rebase(kNavStreamManagerGlobal, sizeof(void*));
    const auto vtable  = image.Rebase(kFollowStatePlayerVTable);
    const bool frame   = image.BytesMatch(kLocationArgument, kLocationArgumentBytes) &&
                       image.BytesMatch(kAfterCall, kAfterCallBytes);
    if (!readyAt || !site || !manager || !vtable || !frame ||
        !g_readyCall.Install(*site, *readyAt, reinterpret_cast<std::uintptr_t>(&ReadyAt)))
    {
        platform::Log(L"nav strand: UNAVAILABLE (a check failed); player 2's member being stranded off the navmesh is not "
                      L"logged");
        return false;
    }
    g_navStreamManager  = *manager;
    g_followStateVTable = *vtable;
    g_installed         = true;
    const auto fallSite  = image.Rebase(kFallCall, 5);
    const bool fallFrame = fallSite && image.BytesMatch(kFallGate, kFallGateBytes) &&
                           image.BytesMatch(kFallLocation, kFallLocationBytes) &&
                           image.BytesMatch(kFallAfterCall, kFallAfterCallBytes);
    bool falls = false;
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        if (fallFrame && call_site::Calls(*fallSite, *readyAt))
        {
            g_fallReturn = *fallSite + 5;  // before the install: the replacement runs the moment it is reachable
            falls        = g_readyEntry.Install(
                *readyAt,
                image.EntryInstruction(kReadyAt.address, std::span(kReadyAtBytes).first(kReadyAtFirstInstruction)),
                reinterpret_cast<std::uintptr_t>(&EntryReadyAt));
            g_fallReturn = falls ? g_fallReturn : 0;
        }
    }
    else
    {
        falls = fallFrame && g_fallCall.Install(*fallSite, *readyAt, reinterpret_cast<std::uintptr_t>(&FallReadyAt));
    }
    const auto parkSite = image.Rebase(kParkCall, 5);
    const bool parks    = parkSite && image.BytesMatch(kParkNavFlag, kParkNavFlagBytes) &&
                       image.BytesMatch(kParkLocation, kParkLocationBytes) &&
                       image.BytesMatch(kParkAfterCall, kParkAfterCallBytes) &&
                       g_parkCall.Install(*parkSite, *readyAt, reinterpret_cast<std::uintptr_t>(&SafeCheckReadyAt));
    platform::Log(L"nav strand: watching the game's refusals of player 2's member's moves{}{} (a strand held {:.1f} s "
                  L"warps her back)",
                  falls ? L", falls" : L" (NOT falls: a check failed; a member frozen in the air stays there)",
                  parks ? L" and parks" : L" (NOT parks: a check failed; a member the game parks stays there)",
                  kHoldSeconds);
    return true;
}

void Remove() noexcept
{
    g_installed = false;
    g_memberRoot.store(nullptr, std::memory_order_release);
    g_chocoboRoot.store(nullptr, std::memory_order_release);
    for (auto* call : {&g_readyCall, &g_fallCall, &g_parkCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"nav strand: a call was changed after us; left in place (it passes through)");
        }
    }
    if (g_readyEntry.IsInstalled() && !g_readyEntry.Restore())
    {
        platform::Log(L"nav strand: the ready test's entry was changed after us; left in place (it passes through)");
    }
    Forget();  // a later Install starts from a clean slate
}

void Tick(bool splitShown) noexcept
{
    if (!g_installed)
    {
        return;
    }
    void* member = generation::Active() ? generation::Member() : nullptr;
    if (member != g_member)
    {
        Forget();
        g_member = member;
        if (member != nullptr)
        {
            ProbeAi(L"member taken", true);
        }
    }
    if (member == nullptr)
    {
        MountedTick(nullptr, nullptr, nullptr, splitShown, 0.0f);
        party_warp::Tick(nullptr, nullptr, splitShown, 0.0f);  // lets a warp in progress go
        return;
    }
    g_memberRoot.store(RootOf(member), std::memory_order_release);

    const auto now     = std::chrono::steady_clock::now();
    const float seconds = g_ticked ? std::min(std::chrono::duration<float>(now - g_lastTick).count(), kMaxTickSeconds)
                                   : 0.0f;
    g_lastTick = now;
    g_ticked   = true;
    const void* ridden = chocobo_ride::RiddenChocobo();
    MountedTick(member, chocobo_ride::DrivenChocobo(), ridden, splitShown, seconds);

    const std::uint32_t refusals = platform::Take(g_tickRefusals);
    const float         longest  = g_tickLongestMove.exchange(0.0f, std::memory_order_relaxed);
    const std::uint32_t falls    = platform::Take(g_tickFallRefusals);
    const std::uint32_t unsafe   = platform::Take(g_tickUnsafeChecks);
    // A failed check alone may still end in the game's own return-warp to her safe spot (the collision wall,
    // §6b); it strands her only once the game has parked her.
    const std::uint32_t parkChecks = unsafe > 0 && Parked(member) ? unsafe : 0;

    switch (g_watch.Step(refusals > 0 || parkChecks > 0, seconds))
    {
        case StrandEvent::Began:
            g_strand = {};
            g_strand.started = LocationOf(member, g_strand.start);
            platform::Log(L"nav strand: began: the game {} (the world under her is not loaded), {}",
                          refusals == 0 ? L"parked player 2's member (MOVE_None, stop reason 5)"
                          : falls > 0   ? L"refused player 2's member's fall (she hangs in the air)"
                                        : L"refused player 2's member's move",
                          Places());
            ProbeAi(L"at the strand", true);
            break;
        case StrandEvent::Warp:
            platform::Log(L"nav strand: stranded for {:.2f} s: {} refused move(s) ({} of them falls), {} failed check(s) "
                          L"while parked, longest {:.1f} cm; moved {:.0f} cm since it began; {}",
                          g_watch.Held(), g_strand.refusals + refusals, g_strand.falls + falls,
                          g_strand.parkChecks + parkChecks, std::max(g_strand.longestMove, longest), MovedSinceStart(),
                          Places());
            ProbeAi(L"at the warp", true);
            party_warp::Start(member, party_warp::Subject::Member,
                              ReadPointer(RootOf(member), kSceneComponentAttachParent) != nullptr,
                              splitShown);  // logs its own outcome
            break;
        case StrandEvent::Ended:
            EndStrand(L"no refusal for a moment: stick let go, the world is back, or she was brought back");
            break;
        case StrandEvent::None: ProbeAi(L"changed", false); break;
    }
    party_warp::Tick(member, ridden, splitShown, seconds);
    if (g_watch.Stranded())
    {
        g_strand.refusals += refusals;
        g_strand.falls += falls;
        g_strand.parkChecks += parkChecks;
        g_strand.longestMove = std::max(g_strand.longestMove, longest);
    }

    if (!g_loggedOffThread && g_offThread.load(std::memory_order_relaxed) > 0)
    {
        g_loggedOffThread = true;
        platform::Log(L"nav strand: the game answers for player 2's member's moves on a worker thread too (counted)");
    }
    if (const auto ready = g_readyNoNavmesh.load(std::memory_order_relaxed);
        ready >= g_loggedReadyNoNavmesh + 600 && platform::Enabled(platform::Level::Debug))
    {
        g_loggedReadyNoNavmesh = ready;  // advanced only by a written line
        {
            Line(L"{} of player 2's member's moves found no navmesh where the world was ready (not refused so far); {}",
                 ready, Places());
        }
    }
}

}  // namespace lss::player_two::nav_strand
