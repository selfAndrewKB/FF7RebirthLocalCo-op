#include "world/nav_strand.h"
#include "battle/arena_warp.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#include "platform/dev_switch.h"
#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "input/pad_buttons.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/call_site.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "rides/chocobo_ride.h"
#include "world/party_warp.h"
#include "world/party_warp_rules.h"
#include "world/strand_watch.h"

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

namespace lss::world::nav_strand {

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
constexpr std::uint8_t   kLocationArgumentBytes[] = {0x48, 0x8D, 0x94, 0x24, 0xB8, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kLocationArgument{L"the move refusal's location argument", 0x140A53846,
                                                   kLocationArgumentBytes};
constexpr std::uint8_t   kAfterCallBytes[] = {0x84, 0xC0, 0x75, 0x2D, 0xC5, 0xFA, 0x11, 0xB4, 0x24, 0x80, 0x00, 0x00,
                                              0x00, 0xC5, 0xFA, 0x11, 0xB4, 0x24, 0x84, 0x00, 0x00, 0x00, 0xC5, 0xFA,
                                              0x11, 0xB4, 0x24, 0x88, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x10, 0x84, 0x24,
                                              0x80, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x11, 0x84, 0x24, 0x90, 0x00, 0x00,
                                              0x00};
constexpr platform::FunctionFact kAfterCall{L"after the move refusal's call", 0x140A538CB, kAfterCallBytes};
constexpr std::ptrdiff_t kMoveBelowLocation = 0xB8 - 0x90;

// The falling step's call (RE-p2-party-leash.md §6c): EndCMC_NewFallVelocity_WorldReadyFreeze asks the same
// question with the same arguments when CMC+0xB6B is set (`cmp byte [rcx+0B6Bh], 0` before it; `lea rdx,
// [rbp-51h]`, a copy of the updated component's world location) and on false returns a zero velocity.
constexpr std::uintptr_t kFallCall          = 0x140F22706;
constexpr std::uint8_t   kFallGateBytes[]   = {0x80, 0xB9, 0x6B, 0x0B, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kFallGate{L"the falling step's CMC+0xB6B gate", 0x140F22659, kFallGateBytes};
constexpr std::uint8_t   kFallLocationBytes[] = {0x48, 0x8D, 0x55, 0xAF};
constexpr platform::FunctionFact kFallLocation{L"the falling step's location argument", 0x140F226BD,
                                               kFallLocationBytes};
constexpr std::uint8_t   kFallAfterCallBytes[] = {0x84, 0xC0, 0x75, 0x13, 0x48, 0xC7, 0x03, 0x00, 0x00,
                                                  0x00, 0x00, 0xC7, 0x43, 0x08, 0x00, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kFallAfterCall{L"after the falling step's call", 0x140F2270B, kFallAfterCallBytes};
// 1.005 (RE-1005-field-2.md §2): the falling step's call is at 0x140C5FAEE, %16 = 14, so it crosses a 16-byte block,
// and characters move on worker threads too, so no one thread can change it (platform/call_redirect.h). There the
// falls are heard at the entry of EndWorld_IsReadyAt instead (its first instruction `mov [rsp+10h], rdx`, 5 bytes at
// %16 = 8): the replacement counts only the answers that return to the falling step's call and passes every other
// caller through unchanged.
constexpr std::size_t kReadyAtFirstInstruction = 5;

// The safe-transform check (RE-p2-party-leash.md §6d): every 0.12 s EndChar_UpdateSafeTransform_Check asks the same
// question about the character's root with the nav clause off (`xor r8d, r8d`; `lea rdx, [rsp+50h]`, a copy of the
// root's world location; the reason byte zeroed and passed in rcx) and on false marks the spot unsafe (`S+0x70 = 1`).
// The next pawn tick either return-warps the character to its last safe spot or, standing on it, parks it: stop
// reason 5 in its movement owner's stop mask, which forces MOVE_None, so neither call above is asked any more.
constexpr std::uintptr_t kParkCall            = 0x140DECB06;
constexpr std::uint8_t   kParkNavFlagBytes[]  = {0x45, 0x33, 0xC0};
constexpr platform::FunctionFact kParkNavFlag{L"the park's nav-flag argument", 0x140DECAE9, kParkNavFlagBytes};
constexpr std::uint8_t   kParkLocationBytes[] = {0x48, 0x8D, 0x54, 0x24, 0x50, 0xC6, 0x44, 0x24, 0x40,
                                                 0x00, 0x48, 0x8D, 0x4C, 0x24, 0x40};
constexpr platform::FunctionFact kParkLocation{L"the park's location argument", 0x140DECAF1, kParkLocationBytes};
constexpr std::uint8_t   kParkAfterCallBytes[] = {0x84, 0xC0, 0x75, 0x09, 0xC6, 0x43, 0x70,
                                                  0x01, 0xE9, 0xF1, 0x02, 0x00, 0x00};
constexpr platform::FunctionFact kParkAfterCall{L"after the park's call", 0x140DECB0B, kParkAfterCallBytes};
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

constexpr float kMaxTickSeconds = 0.25f;  // a hitch counts as this much, so one frame never completes a hold

// The hold is shorter than the game's 1.0 s for AI followers: a refusal already means the navmesh under the
// character is not loaded, and a player running in place should not wait; 0.3 s only rides out a cell that is
// still loading. The cooldown is the game's; the grace is StrandWatch's default (spans refused-fall bursts).
constexpr float kHoldSeconds = 0.3f;
// A strand warp the warp refused (a scene holding the field, player 1 on a ladder) is asked again this often while the
// strand holds, as the game's follower keeps trying its own.
constexpr float kRetrySeconds = 2.0f;

using ReadyAtFn = bool (*)(void* reason, const float* location, bool flag);

// Set once at install.
platform::CallRedirect  g_readyCall;
platform::CallRedirect  g_fallCall;
platform::CallRedirect  g_parkCall;
platform::EntryRedirect g_readyEntry;              // 1.005: the falls, heard at the entry
std::uintptr_t          g_fallReturn       = 0;    // 1.005: live return address of the falling step's call
std::uintptr_t          g_navStreamManager = 0;    // live address of the global
bool                    g_installed        = false;  // the strands are watched (the asks need only the warp)

// A player's ask (game thread), by seat: the L3 double click; the console's ask, made at the next tick; an ask waiting
// for another seat's warp to finish (how it was asked; null: none).
party_warp_rules::DoubleClick g_clicks[players::kMaxPlayers];
bool                          g_consoleAsk[players::kMaxPlayers] = {};
const wchar_t*                g_askWaiting[players::kMaxPlayers] = {};

/// What a strand in progress has gathered.
struct Strand
{
    std::uint32_t refusals    = 0;
    std::uint32_t falls       = 0;  // of the refusals, refused falls
    std::uint32_t parkChecks  = 0;  // failed safe-transform checks while the game held the character parked
    float         longestMove = 0.0f;
    float         start[3]{};
    bool          started     = false;  // `start` was read
};

/// The game's answers since the last tick.
struct Counts
{
    std::uint32_t refusals = 0;     // refused moves (the falls among them)
    std::uint32_t falls    = 0;
    std::uint32_t unsafe   = 0;     // failed safe-transform checks
    float         longest  = 0.0f;  // the longest refused move, cm
};

/// One of the two characters a seat's strand is watched on: the seat's member on foot, or the chocobo the seat's player
/// rides (its moves go through the same three calls; its warp is its own follow warp, with the player seated).
struct Subject
{
    const wchar_t*      who;
    party_warp::Subject kind;
    int                 seat;
    // Written on the game thread, read by the redirects on whichever thread moves the character (the game moves
    // characters on worker threads too, so the redirects only count, from any thread).
    std::atomic<const std::byte*> root{nullptr};  // the root of the character whose answers are counted; null: none
    std::atomic<std::uint32_t>    refusals{0};
    std::atomic<std::uint32_t>    falls{0};
    std::atomic<std::uint32_t>    unsafeChecks{0};
    std::atomic<float>            longestMove{0.0f};
    // Game thread.
    StrandWatch watch{StrandRules{.holdSeconds = kHoldSeconds}};
    Strand      strand;
    const void* pawn        = nullptr;  // the character whose strand is watched; null: none (the counts are dropped)
    bool        warpWaiting = false;    // its warp waits for another seat's warp to finish (one at a time)
    float       retryIn     = 0.0f;     // seconds until a refused strand warp is asked again (0: none)
#if LSS_DEV
    platform::Every status{std::chrono::seconds(1)};  // the once-a-second status line (dev tools)
    Counts          second;                            // its counts
    bool            quiet = true;                      // the last status line had nothing to report
    std::wstring    probed;                            // the AI probe's last text
#endif
};

// By seat (players/seats.h; index 0, player 1's, never aimed). The member: counted and watched as one. The chocobo:
// counted while the seat's player rides it (getting off too), its strand watched only while seated
// (RE-p2-chocobo-warp.md §1, §4).
Subject g_members[players::kMaxPlayers] = {{L"player 1's member", party_warp::Subject::Member, 0},
                                           {L"player 2's member", party_warp::Subject::Member, 1},
                                           {L"player 3's member", party_warp::Subject::Member, 2}};
Subject g_chocobos[players::kMaxPlayers] = {{L"player 1's chocobo", party_warp::Subject::Chocobo, 0},
                                            {L"player 2's chocobo", party_warp::Subject::Chocobo, 1},
                                            {L"player 3's chocobo", party_warp::Subject::Chocobo, 2}};
// The texts a change of the watched character closes a strand with, by seat.
constexpr const wchar_t* kMemberChanged[players::kMaxPlayers] = {
    L"player 1's member changed or left", L"player 2's member changed or left", L"player 3's member changed or left"};
constexpr const wchar_t* kNotSeated[players::kMaxPlayers] = {
    L"player 1 is no longer seated on it, or rides another", L"player 2 is no longer seated on it, or rides another",
    L"player 3 is no longer seated on it, or rides another"};

// The chocobos a seat's player drives (counted) and rides seated (watched).
const void* DrivenChocoboOf(int seat) noexcept
{
    return rides::chocobo_ride::DrivenChocobo(seat);
}

const void* RiddenChocoboOf(int seat) noexcept
{
    return rides::chocobo_ride::RiddenChocobo(seat);
}

// For the whole session (logged as they grow):
std::atomic<std::uint32_t> g_readyNoNavmesh{0};  // moves that found no navmesh where the world was ready (not refused)
std::atomic<std::uint32_t> g_offThread{0};       // answers that came off the game thread
std::chrono::steady_clock::time_point g_lastTick[players::kMaxPlayers]{};  // by seat: its last tick with a member
bool                                  g_ticked[players::kMaxPlayers]  = {};
bool                                  g_loggedOffThread = false;
#if LSS_DEV
std::uint32_t g_loggedReadyNoNavmesh = 0;

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
#endif

const std::byte* At(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? nullptr : static_cast<const std::byte*>(object) + offset;
}

/// True when `location` is the character's whose root is `rootOf`. The call sites pass no component, only a copy of
/// its world location, so the character is recognised by an exact match with its root's; another character would
/// have to stand on the very same floats.
bool IsAt(const std::atomic<const std::byte*>& rootOf, const float* location) noexcept
{
    const std::byte* root = rootOf.load(std::memory_order_acquire);
    float            at[3]{};
    return root != nullptr && platform::CopyGuarded(at, root + kComponentWorldLocation, sizeof(at)) &&
           std::memcmp(at, location, sizeof(at)) == 0;
}

/// The subject standing at `location`, or null (any thread). The seats in order, each its member, then its chocobo.
Subject* SubjectAt(const float* location) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (IsAt(g_members[seat].root, location))
        {
            return &g_members[seat];
        }
        if (IsAt(g_chocobos[seat].root, location))
        {
            return &g_chocobos[seat];
        }
    }
    return nullptr;
}

void NoteThread() noexcept
{
    if (!players::generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Counts one of a subject's answers; the subject if it was a refusal, else null.
Subject* Refused(const float* location, bool ready) noexcept
{
    Subject* subject = SubjectAt(location);
    if (subject == nullptr)
    {
        return nullptr;
    }
    NoteThread();
    if (ready)
    {
        g_readyNoNavmesh.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    subject->refusals.fetch_add(1, std::memory_order_relaxed);
    return subject;
}

/// The walking redirect: the game's answer, unchanged; a refusal zeroes the character's move.
bool ReadyAt(void* reason, const float* location, bool flag) noexcept
{
    const bool ready   = reinterpret_cast<ReadyAtFn>(g_readyCall.Original())(reason, location, flag);
    Subject*   subject = Refused(location, ready);
    float      move[3]{};
    if (subject != nullptr &&
        platform::CopyGuarded(move, reinterpret_cast<const std::byte*>(location) - kMoveBelowLocation, sizeof(move)))
    {
        const float length  = std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
        float       longest = subject->longestMove.load(std::memory_order_relaxed);
        while (length > longest &&
               !subject->longestMove.compare_exchange_weak(longest, length, std::memory_order_relaxed))
        {
        }
    }
    return ready;
}

/// The falling redirect: the game's answer, unchanged; a refusal zeroes the character's fall velocity, so it hangs in
/// the air, every tick, stick or no stick.
void CountFall(const float* location, bool ready) noexcept
{
    if (Subject* subject = Refused(location, ready); subject != nullptr)
    {
        subject->falls.fetch_add(1, std::memory_order_relaxed);
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

/// The safe-transform redirect: the game's answer, unchanged. Only counted; whether it strands the character is the
/// game's own park bit (Parked), read on the game thread.
bool SafeCheckReadyAt(void* reason, const float* location, bool flag) noexcept
{
    const bool ready = reinterpret_cast<ReadyAtFn>(g_parkCall.Original())(reason, location, flag);
    if (Subject* subject = ready ? nullptr : SubjectAt(location); subject != nullptr)
    {
        NoteThread();
        subject->unsafeChecks.fetch_add(1, std::memory_order_relaxed);
    }
    return ready;
}

/// The game holds the character parked (stop reason 5, MOVE_None) until a safe-transform check passes.
bool Parked(const void* pawn) noexcept
{
    std::uint32_t mask = 0;
    return Read(At(ReadPointer(pawn, kCharacterMoveOwner), kOwnerStopMask), mask) && (mask & kStopReasonParked) != 0;
}

std::wstring AtText(const void* pawn) noexcept
{
    float at[3]{};
    try
    {
        return players::unreal::ActorLocation(pawn, at) ? std::format(L"({:.0f}, {:.0f}, {:.0f})", at[0], at[1], at[2]) : L"?";
    }
    catch (...)
    {
        return {};
    }
}

std::wstring PlacesText(int seat)
{
    float one[3]{}, member[3]{}, cell[3]{};
    const void* leader    = ReadPointer(players::generation::PlayerOneController(), facts::kControllerPawn);
    const bool  hasOne    = players::unreal::ActorLocation(leader, one);
    const bool  hasMember = players::unreal::ActorLocation(g_members[seat].pawn, member);
    const void* manager = nullptr;
    const bool  hasCell = Read(reinterpret_cast<const void*>(g_navStreamManager), manager) &&
                         Read(At(manager, kNavStreamLeaderCell), cell);
    std::wstring text = hasMember ? std::format(L"P{} ({:.0f}, {:.0f}, {:.0f})", seat + 1, member[0], member[1],
                                                member[2])
                                  : std::format(L"P{} ?", seat + 1);
    text += hasOne ? std::format(L", P1 ({:.0f}, {:.0f}, {:.0f})", one[0], one[1], one[2]) : L", P1 ?";
    if (hasOne && hasMember)
    {
        text += std::format(L", apart {:.0f} m", std::hypot(member[0] - one[0], member[1] - one[1]) / 100.0f);
    }
    text += hasCell ? std::format(L", leader nav cell ({:.0f}, {:.0f}, {:.0f})", cell[0], cell[1], cell[2])
                    : L", leader nav cell ?";
    return text;
}

/// The seat's player's and player 1's positions, their distance and the leader's nav cell, for a log line.
std::wstring Places(int seat) noexcept
{
    try
    {
        return PlacesText(seat);
    }
    catch (...)
    {
        return {};
    }
}

/// How far the character moved since its strand began (the running-in-place check), cm; -1 unknown.
float MovedSinceStart(const Subject& subject) noexcept
{
    float now[3]{};
    return subject.strand.started && players::unreal::ActorLocation(subject.pawn, now)
               ? std::hypot(now[0] - subject.strand.start[0], now[1] - subject.strand.start[1])
               : -1.0f;
}

/// The counts since the last tick, taken (game thread).
Counts Take(Subject& subject) noexcept
{
    return {platform::Take(subject.refusals), platform::Take(subject.falls), platform::Take(subject.unsafeChecks),
            subject.longestMove.exchange(0.0f, std::memory_order_relaxed)};
}

void Add(Strand& strand, const Counts& counts, std::uint32_t parkChecks) noexcept
{
    strand.refusals += counts.refusals;
    strand.falls += counts.falls;
    strand.parkChecks += parkChecks;
    strand.longestMove = std::max(strand.longestMove, counts.longest);
}

#if LSS_DEV
/// The character's AI when it changed, or with `always` (Debug): whether its state is still the follow state the
/// game's warp lives in.
void Probe(Subject& subject, const wchar_t* when, bool always) noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || subject.pawn == nullptr)
    {
        return;
    }
    std::wstring text = party_warp::AiText(subject.pawn, subject.kind);
    if (!always && text == subject.probed)
    {
        return;
    }
    subject.probed = std::move(text);
    Line(L"{} ({}): its {}", subject.who, when, subject.probed);
}

/// Once a second while something was counted or the game holds the character parked: the second's counts (Debug).
void Status(Subject& subject, const Counts& counts) noexcept
{
    subject.second.refusals += counts.refusals;
    subject.second.falls += counts.falls;
    subject.second.unsafe += counts.unsafe;
    subject.second.longest = std::max(subject.second.longest, counts.longest);
    if (!subject.status.Due(std::chrono::steady_clock::now()))
    {
        return;
    }
    const Counts second = std::exchange(subject.second, Counts{});
    if (!platform::Enabled(platform::Level::Debug))
    {
        subject.quiet = true;
        return;  // nothing is read for the line below that level
    }
    const bool parked = Parked(subject.pawn);
    const bool quiet  = second.refusals == 0 && second.falls == 0 && second.unsafe == 0 && !parked;
    if (quiet && subject.quiet)
    {
        return;
    }
    subject.quiet = quiet;
    Line(L"{} in the last second: {} refused move(s) ({} of them falls, longest {:.1f} cm), {} failed safe check(s); "
         L"parked {}; at {}; its {}; {}",
         subject.who, second.refusals, second.falls, second.longest, second.unsafe, parked ? L"yes" : L"no",
         AtText(subject.pawn), party_warp::AiText(subject.pawn, subject.kind), Places(subject.seat));
}
#else
void Probe(Subject&, const wchar_t*, bool) noexcept {}  // the AI probe is a dev tool
void Status(Subject&, const Counts&) noexcept {}        // so is the status line
#endif

/// A strand's end (Info: a rare, real state change).
void LogEnd(const Subject& subject, const wchar_t* why) noexcept
{
    platform::Log(L"nav strand: {}'s strand ended ({}) after {:.2f} s: {} refused move(s) ({} of them falls), {} "
                  L"failed check(s) while parked, longest {:.1f} cm; moved {:.0f} cm since it began, now at {}, "
                  L"parked {}; {}",
                  subject.who, why, subject.watch.Held(), subject.strand.refusals, subject.strand.falls,
                  subject.strand.parkChecks, subject.strand.longestMove, MovedSinceStart(subject), AtText(subject.pawn),
                  Parked(subject.pawn) ? L"yes" : L"no", Places(subject.seat));
}

/// Ends a strand in progress (with the last tick's counts, not yet taken) and forgets the character.
void Close(Subject& subject, const wchar_t* why) noexcept
{
    const Counts counts = Take(subject);
    if (subject.watch.Stranded())
    {
        Add(subject.strand, counts, ParkChecks(counts.unsafe, counts.unsafe > 0 && Parked(subject.pawn)));
        LogEnd(subject, why);
    }
    subject.watch.Reset();
    subject.strand      = {};
    subject.pawn        = nullptr;
    subject.warpWaiting = false;
    subject.retryIn     = 0.0f;
#if LSS_DEV
    subject.second = {};
    subject.quiet  = true;
    subject.probed.clear();
#endif
}

/// Points the subject at its characters: `counted`'s answers are counted, `watched`'s strand is watched (the member:
/// the same; the chocobo: the one the seat's player rides, and the one they sit on). A change of the watched one
/// closes its strand with `why`.
void Aim(Subject& subject, const void* counted, const void* watched, const wchar_t* why) noexcept
{
    subject.root.store(players::unreal::RootOf(counted), std::memory_order_release);
    if (watched != subject.pawn)
    {
        Close(subject, why);
        subject.pawn = watched;
    }
}

/// Asks the game's follow warp to bring the subject back (party_warp logs its own outcome). While another seat's warp
/// is being driven the request waits (logged) and is made once that warp is over, if the strand still holds; with one
/// seat it never waits. False when the warp refused it (logged by party_warp).
bool RequestWarp(Subject& subject, bool splitShown) noexcept
{
    const int busy = party_warp::BusySeat();
    if (busy == subject.seat)
    {
        subject.warpWaiting = false;
        return true;  // the seat's warp is already being driven (its player's ask, or its other character's strand)
    }
    if (players::SeatBuilt(busy))
    {
        if (!subject.warpWaiting)
        {
            subject.warpWaiting = true;
            platform::Log(L"nav strand: {}'s warp waits for {}'s warp to finish", subject.who, players::SeatName(busy));
        }
        return true;
    }
    subject.warpWaiting = false;
    return party_warp::Start(subject.seat, const_cast<void*>(subject.pawn), subject.kind,
                             ReadPointer(players::unreal::RootOf(subject.pawn), kSceneComponentAttachParent) != nullptr, splitShown);
}

/// The seat's player asked to be brought back (`how`: L3 twice, the console): the follow warp of the chocobo they ride,
/// else of their member, unless it already stands beside player 1's character (player 1's pawn: their character, or
/// the mount they ride, at the same spot). While another seat's warp is being driven the ask waits and is asked again,
/// "beside" included, once that warp is over. party_warp logs the rest of the answer.
void Ask(int seat, const wchar_t* how, bool splitShown) noexcept
{
    g_askWaiting[seat] = nullptr;
    if (battle::arena_warp::InBattle())
    {
        battle::arena_warp::Ask(seat, how);  // in a fight the same ask brings the character into the fight's area
        return;
    }
    Subject& subject   = g_chocobos[seat].pawn != nullptr ? g_chocobos[seat] : g_members[seat];
    if (subject.pawn == nullptr)
    {
        platform::Log(L"party warp: {} asked to be brought back ({}); not done: no character", players::SeatName(seat), how);
        return;
    }
    float       one[3]{}, at[3]{};
    const void* leader = ReadPointer(players::generation::PlayerOneController(), facts::kControllerPawn);
    if (players::unreal::ActorLocation(leader, one) && players::unreal::ActorLocation(subject.pawn, at) &&
        party_warp_rules::Beside(std::hypot(at[0] - one[0], at[1] - one[1]), at[2] - one[2]))
    {
        platform::Log(L"party warp: {} asked to be brought back ({}); not done: it already stands beside player 1's "
                      L"character; {}",
                      subject.who, how, Places(seat));
        return;
    }
    const int busy = party_warp::BusySeat();
    if (players::SeatBuilt(busy) && busy != seat)
    {
        g_askWaiting[seat] = how;
        platform::Log(L"party warp: {} asked to be brought back ({}); waits for {}'s warp to finish", subject.who, how,
                      players::SeatName(busy));
        return;
    }
    platform::Log(L"party warp: {} asked to be brought back ({}); {}", subject.who, how, Places(seat));
    RequestWarp(subject, splitShown);
}

/// One tick of a subject's strand: the game's answers since the last tick become began / warp / ended.
void Step(Subject& subject, bool splitShown, float seconds) noexcept
{
    const Counts counts = Take(subject);
    if (subject.pawn == nullptr)
    {
        return;  // counted, not watched (the chocobo while the seat's player gets off)
    }
    // A failed check alone may still end in the game's own return-warp to the safe spot (the collision wall,
    // RE-p2-party-leash.md §6b); it strands the character only once the game has parked it.
    const std::uint32_t parkChecks = ParkChecks(counts.unsafe, counts.unsafe > 0 && Parked(subject.pawn));
    Status(subject, counts);
    switch (subject.watch.Step(counts.refusals > 0 || parkChecks > 0, seconds))
    {
        case StrandEvent::Began:
            subject.strand         = {};
            subject.strand.started = players::unreal::ActorLocation(subject.pawn, subject.strand.start);
            platform::Log(L"nav strand: {} stranded: the game {} at {} (the world under it is not loaded); {}",
                          subject.who,
                          counts.refusals == 0 ? L"parked it (MOVE_None, stop reason 5)"
                          : counts.falls > 0   ? L"refused its fall (it hangs in the air)"
                                               : L"refused its move",
                          AtText(subject.pawn), Places(subject.seat));
            Probe(subject, L"at the strand", true);
            break;
        case StrandEvent::Warp:
            platform::Log(L"nav strand: {} stranded for {:.2f} s: {} refused move(s) ({} of them falls), {} failed "
                          L"check(s) while parked, longest {:.1f} cm; moved {:.0f} cm since it began; {}; asking the "
                          L"game's follow warp to bring it back",
                          subject.who, subject.watch.Held(), subject.strand.refusals + counts.refusals,
                          subject.strand.falls + counts.falls, subject.strand.parkChecks + parkChecks,
                          std::max(subject.strand.longestMove, counts.longest), MovedSinceStart(subject),
                          Places(subject.seat));
            Probe(subject, L"at the warp", true);
            subject.retryIn = RequestWarp(subject, splitShown) ? 0.0f : kRetrySeconds;
            break;
        case StrandEvent::Ended:
            subject.warpWaiting = false;
            subject.retryIn     = 0.0f;
            LogEnd(subject, L"no refusal for a moment: stick let go, the world is back, or it was brought back");
            break;
        case StrandEvent::None:
            Probe(subject, L"changed", false);
            if (subject.warpWaiting && party_warp::BusySeat() < 0)
            {
                RequestWarp(subject, splitShown);  // the other seat's warp is over and the strand still holds
            }
            else if (subject.retryIn > 0.0f && subject.watch.Stranded() && (subject.retryIn -= seconds) <= 0.0f)
            {
                subject.retryIn = RequestWarp(subject, splitShown) ? 0.0f : kRetrySeconds;  // refused: asked again
            }
            break;
    }
    if (subject.watch.Stranded())
    {
        Add(subject.strand, counts, parkChecks);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto readyAt = platform::FindFunction(image, kReadyAt);
    const auto site    = image.Rebase(kReadyCall, 5);
    const auto manager = image.Rebase(kNavStreamManagerGlobal, sizeof(void*));
    const bool frame   = platform::FindFunction(image, kLocationArgument) && platform::FindFunction(image, kAfterCall);
    if (!readyAt || !site || !manager || !frame ||
        !g_readyCall.Install(*site, *readyAt, reinterpret_cast<std::uintptr_t>(&ReadyAt)))
    {
        platform::Log(L"nav strand: UNAVAILABLE (a check failed); {} member being stranded off the navmesh is not logged "
                      L"or warped back (a player's own ask still is)",
                      players::kSeatsBuilt == 1 ? L"player 2's" : L"player 2's or 3's");
        return false;
    }
    g_navStreamManager = *manager;
    g_installed        = true;
    const auto fallSite  = image.Rebase(kFallCall, 5);
    const bool fallFrame = fallSite && platform::FindFunction(image, kFallGate) &&
                           platform::FindFunction(image, kFallLocation) &&
                           platform::FindFunction(image, kFallAfterCall);
    bool falls = false;
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        if (fallFrame && players::call_site::Calls(*fallSite, *readyAt))
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
    const bool parks    = parkSite && platform::FindFunction(image, kParkNavFlag) &&
                       platform::FindFunction(image, kParkLocation) && platform::FindFunction(image, kParkAfterCall) &&
                       g_parkCall.Install(*parkSite, *readyAt, reinterpret_cast<std::uintptr_t>(&SafeCheckReadyAt));
    platform::Log(L"nav strand: watching the game's refusals of the moves{}{} of {} member and of the chocobo they ride "
                  L"(a strand held {:.1f} s warps it back)",
                  falls ? L", falls" : L" (NOT falls: a check failed; a character frozen in the air stays there)",
                  parks ? L" and parks" : L" (NOT parks: a check failed; a character the game parks stays there)",
                  players::kSeatsBuilt == 1 ? L"player 2's" : L"player 2's or 3's", kHoldSeconds);
    return true;
}

void Remove() noexcept
{
    g_installed = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        g_members[seat].root.store(nullptr, std::memory_order_release);
        g_chocobos[seat].root.store(nullptr, std::memory_order_release);
    }
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
    // A later Install starts from a clean slate.
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Close(g_members[seat], L"the mod is leaving");
        Close(g_chocobos[seat], L"the mod is leaving");
        g_ticked[seat]     = false;
        g_clicks[seat]     = {};
        g_consoleAsk[seat] = false;
        g_askWaiting[seat] = nullptr;
    }
}

void Tick(bool splitShown) noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    // Each seat in order: its member and its chocobo aimed, its player's ask heard, its strands stepped (when watched);
    // then the one warp driven (party_warp's seat).
    party_warp::SeatPawns members{}, ridden{};
    float                 seconds[players::kMaxPlayers] = {};
    const auto            now = std::chrono::steady_clock::now();
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Subject&   memberSubject = g_members[seat];
        void*      member        = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        const bool taken         = member != nullptr && member != memberSubject.pawn;
        Aim(memberSubject, member, member, kMemberChanged[seat]);
        if (taken)
        {
            Probe(memberSubject, L"member taken", true);
        }
        const void* driven = member == nullptr ? nullptr : DrivenChocoboOf(seat);
        ridden[seat]       = member == nullptr ? nullptr : RiddenChocoboOf(seat);
        members[seat]      = member;
        Aim(g_chocobos[seat], driven, ridden[seat], kNotSeated[seat]);
        seconds[seat] = g_ticked[seat] && member != nullptr
                            ? std::min(std::chrono::duration<float>(now - g_lastTick[seat]).count(), kMaxTickSeconds)
                            : 0.0f;
        g_lastTick[seat] = now;
        g_ticked[seat]   = member != nullptr;
        const bool pressed = input::pad_buttons::TakeLeftThumbPress(seat);  // taken every tick: none carried over
        if (member == nullptr || taken)
        {
            g_clicks[seat]     = {};  // a new player or member starts afresh
            g_askWaiting[seat] = nullptr;
        }
        const bool clicked = member != nullptr && pressed &&
                             g_clicks[seat].Press(static_cast<std::uint64_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
        const bool asked = std::exchange(g_consoleAsk[seat], false);
        if (clicked || asked)
        {
            Ask(seat, clicked ? L"L3 twice" : L"the console", splitShown);
        }
        else if (g_askWaiting[seat] != nullptr && party_warp::BusySeat() < 0)
        {
            Ask(seat, g_askWaiting[seat], splitShown);  // the other seat's warp is over: asked again, "beside" too
        }
        if (g_installed)
        {
            Step(memberSubject, splitShown, seconds[seat]);
            Step(g_chocobos[seat], splitShown, seconds[seat]);
        }
    }
    // With no member for its seat, lets a warp in progress go.
    const int warping = party_warp::BusySeat();
    party_warp::Tick(members, ridden, splitShown, players::SeatBuilt(warping) ? seconds[warping] : 0.0f);

    if (!g_loggedOffThread && g_offThread.load(std::memory_order_relaxed) > 0)
    {
        g_loggedOffThread = true;
        platform::Log(L"nav strand: the game answers for {}'s characters' moves on a worker thread too (counted)",
                      players::kSeatsBuilt == 1 ? L"player 2" : L"players 2 and 3");
    }
#if LSS_DEV
    if (const auto ready = g_readyNoNavmesh.load(std::memory_order_relaxed);
        ready >= g_loggedReadyNoNavmesh + 600 && platform::Enabled(platform::Level::Debug))
    {
        g_loggedReadyNoNavmesh = ready;  // advanced only by a written line
        std::wstring places;  // of each seat whose player is in
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (players::generation::Active(seat))
            {
                places += (places.empty() ? L"" : L"; ") + Places(seat);
            }
        }
        Line(L"{} of {}'s characters' moves found no navmesh where the world was ready (not refused so far); {}", ready,
             players::kSeatsBuilt == 1 ? L"player 2" : L"players 2 and 3", places);
    }
#endif
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"party-warp",
                       .shape = control::Shape::Integer,
                       .min   = 2,
                       .max   = players::kSeatsBuilt + 1,
                       .help  = L"asks the game's follow warp to bring player 2's or 3's character back beside player "
                                L"1's, as L3 clicked twice on that player's pad does (made at the next tick; the answer "
                                L"in the log)",
                       .set   = [](const Value& value) {
                           const int seat = static_cast<int>(std::get<std::int64_t>(value)) - 1;
                           if (!players::generation::Active(seat))
                           {
                               return Outcome{false, L"that player is not in"};
                           }
                           g_consoleAsk[seat] = true;
                           return Outcome{};
                       }});
}

#endif

}  // namespace lss::world::nav_strand
