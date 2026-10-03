#include "player_two/party_warp.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lss::player_two::party_warp {

namespace {

// The follow-state variants whose four warp slots hold the functions the research read (RE-p2-party-warp.md §1 and
// §7 for PLAYER; RE-p2-chocobo-warp.md §2.1 for the chocobo sub-types). Each is checked at install, and a state is
// driven only if its vtable is a checked one of its subject's kind.
constexpr std::uint32_t kTeleportQuerySlot = 0x58;
constexpr std::uint32_t kForceTeleportSlot = 0x270;
constexpr std::uint32_t kFinishNowSlot     = 0x278;
constexpr std::uint32_t kUpdateTeleportSlot = 0x298;

struct VariantFact
{
    const wchar_t* name;
    Subject        subject;
    std::uintptr_t vtable;
    std::uintptr_t query;   // +0x58  FollowState_TickTeleportEQS_Vt58 (or its wrapper)
    std::uintptr_t force;   // +0x270 FollowState_ForceTeleport_Vt270
    std::uintptr_t finish;  // +0x278 FollowState_FinishForcedTeleportNow_Vt278
    std::uintptr_t update;  // +0x298 the sub-type's teleport update
    bool           skipGate;  // the update returns at once while the AI's +0xAD8 is set (PLAYER only)
};

constexpr VariantFact kVariants[] = {
    {L"PLAYER", Subject::Member, 0x14580C8F8, 0x140CB9618, 0x14183BD94, 0x1431CF7D4, 0x140B1975C, true},
    {L"CHOCOBO_MEMBER", Subject::Chocobo, 0x145CB2C00, 0x140CB9618, 0x14183BD94, 0x1431CF7D4, 0x1431D851C, false},
    {L"CHOCOBO_SUB", Subject::Chocobo, 0x145CB1650, 0x140CB9618, 0x14183BD94, 0x1431CF7D4, 0x1431D851C, false},
    {L"CHOCOBO", Subject::Chocobo, 0x145CB2918, 0x1431C9638, 0x14183BD94, 0x1431CF7D4, 0x1431D84F4, false},
};
constexpr std::size_t kVariantCount = sizeof(kVariants) / sizeof(kVariants[0]);

// PartyManager_IsIdle (bool()), the guard PartyFollow_ForceTeleportAll uses before it forces anyone.
constexpr std::uint8_t kPartyIdleBytes[] = {0x48, 0x8B, 0x05, 0x99, 0x11, 0x88, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x07,
                                            0x80, 0x38, 0x00, 0x0F, 0x94, 0xC0, 0xC3, 0xB0, 0x01, 0xC3, 0xCC, 0xCC};
constexpr platform::FunctionFact kPartyIdle{L"PartyManager_IsIdle", 0x140B1AB40, kPartyIdleBytes};

constexpr std::uint32_t kAiBrain          = 0x518;
constexpr std::uint32_t kAiSkipsUpdate    = 0xAD8;  // the PLAYER update returns at once while set
constexpr std::uint32_t kBrainState       = 0x20;
constexpr std::uint32_t kStateType        = 0x18;
constexpr std::uint32_t kStateForced      = 0x39D;  // forced teleport pending; cleared by the commit
constexpr std::uint32_t kCharacterWarping = 0xB59;  // a warp request is queued on the character
constexpr std::uint8_t  kStateFollow      = 7;
// The teleport query runner (RE-p2-party-warp.md §10): the result pointer and its status (1 = done).
constexpr std::uint32_t kStateQueryResult = 0x540;
constexpr std::uint32_t kQueryStatus      = 0x28;
constexpr std::uintptr_t kIdaImageBase    = 0x140000000;

/// A forced request left unfinished this long is completed by the game's own "finish now". The game's
/// fast-travel wait allows 20 s; a stranded player should not wait that long, and the EQS, the 0.2 s fade
/// and the commit take a fraction of this when they work.
constexpr float kFinishAfterSeconds = 3.0f;

/// The force parameters ForceTeleport copies (mode @0, FTransform @0x10, bFade @0x40).
struct alignas(16) FollowTeleportParams
{
    std::uint8_t mode = 0;  // 0: the normal follow warp (FollowTeleportEQS)
    std::uint8_t padding0[15]{};
    float        rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float        translation[4]{};
    float        scale[4]{1.0f, 1.0f, 1.0f, 0.0f};
    std::uint8_t fade = 1;  // fade out before the warp, as the game's forced mode can
    std::uint8_t padding1[15]{};
};
static_assert(offsetof(FollowTeleportParams, rotation) == 0x10);
static_assert(offsetof(FollowTeleportParams, fade) == 0x40);
static_assert(sizeof(FollowTeleportParams) == 0x50);

using ForceFn  = void (*)(void* state, const FollowTeleportParams* params);
using UpdateFn = void (*)(void* state, float seconds);  // also the query slot's type
using FinishFn = void (*)(void* state);
using TestFn   = bool (*)();

struct Variant
{
    const VariantFact* fact   = nullptr;
    std::uintptr_t     vtable = 0;  // live; 0 when its check failed
    ForceFn            force  = nullptr;
    UpdateFn           update = nullptr;
    UpdateFn           query  = nullptr;
    FinishFn           finish = nullptr;
};

// Set once at install.
Variant        g_variants[kVariantCount];
TestFn         g_partyIdle = nullptr;
TestFn         g_inBattle  = nullptr;
std::uintptr_t g_imageBase = 0;
bool           g_installed = false;

// Game thread.
void*          g_state   = nullptr;  // the follow state being driven
const Variant* g_variant = nullptr;  // its variant
const void*    g_pawn    = nullptr;  // whose state it is (the member, or the chocobo)
Subject        g_subject = Subject::Member;
float          g_elapsed = 0.0f;
int            g_updates = 0;

template <class T>
bool Read(const void* object, std::uint32_t offset, T& out) noexcept
{
    return object != nullptr && platform::CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

void* PointerAt(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    return Read(object, offset, value) ? value : nullptr;
}

std::uint8_t ByteAt(const void* object, std::uint32_t offset) noexcept
{
    std::uint8_t value = 0xFF;
    Read(object, offset, value);
    return value;
}

const wchar_t* Who(Subject subject) noexcept
{
    return subject == Subject::Member ? L"player 2's member" : L"player 2's chocobo";
}

/// The checked variant `state`'s vtable is, for `subject`; null if none.
const Variant* VariantOf(const void* state, Subject subject) noexcept
{
    std::uintptr_t vtable = 0;
    if (!Read(state, 0, vtable) || vtable == 0)
    {
        return nullptr;
    }
    for (const Variant& variant : g_variants)
    {
        if (variant.vtable == vtable && variant.fact->subject == subject)
        {
            return &variant;
        }
    }
    return nullptr;
}

/// The pawn's retained AI's current state and its variant, if it is a checked follow state for `subject`.
void* FollowStateOf(const void* pawn, Subject subject, const Variant** variantOut = nullptr,
                    const void** aiOut = nullptr) noexcept
{
    void* ai    = PointerAt(pawn, facts::kCharacterRetainedAi);
    void* state = PointerAt(PointerAt(ai, kAiBrain), kBrainState);
    if (aiOut != nullptr)
    {
        *aiOut = ai;
    }
    const Variant* variant = ByteAt(state, kStateType) == kStateFollow ? VariantOf(state, subject) : nullptr;
    if (variantOut != nullptr)
    {
        *variantOut = variant;
    }
    return variant != nullptr ? state : nullptr;
}

/// A live vtable as its IDA address, for a log line (0 when unreadable).
std::uintptr_t IdaVtableOf(const void* object) noexcept
{
    std::uintptr_t vtable = 0;
    return Read(object, 0, vtable) && vtable >= g_imageBase ? vtable - g_imageBase + kIdaImageBase : 0;
}

bool Test(TestFn fn, bool& out) noexcept
{
    return platform::CallGuarded(fn, out);
}

void Stop() noexcept
{
    g_state   = nullptr;
    g_variant = nullptr;
    g_pawn    = nullptr;
    g_elapsed = 0.0f;
    g_updates = 0;
}

/// The follow query's status for a log line: 1 = a result is ready; -1 = no result object (the query never ran).
int QueryStatus() noexcept
{
    const void* result = PointerAt(g_state, kStateQueryResult);
    std::uint8_t status = 0;
    return result == nullptr ? -1 : Read(result, kQueryStatus, status) ? status : -2;
}

/// The game's own completion of a forced request (warp to the follow target, fade in).
void FinishNow(const wchar_t* why) noexcept
{
    const int  query = QueryStatus();
    const bool ok    = platform::CallGuarded(g_variant->finish, g_state);
    platform::Log(L"party warp: {} ({}) {} after {:.2f} s ({} update(s), query status {}); the game's finish-now called{}",
                  Who(g_subject), g_variant->fact->name, why, g_elapsed, g_updates, query, ok ? L"" : L" and FAULTED");
    Stop();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto is = [&image](const std::optional<std::uintptr_t>& live, std::uintptr_t ida) {
        const auto expected = image.Rebase(ida);
        return live && expected && *live == *expected;
    };
    int chocoboVariants = 0;
    for (std::size_t i = 0; i < kVariantCount; ++i)
    {
        const VariantFact& fact = kVariants[i];
        const auto slot   = [&image, &fact](std::uint32_t offset) { return image.ReadPointer(fact.vtable + offset); };
        const auto vtable = image.Rebase(fact.vtable);
        const auto force  = slot(kForceTeleportSlot);
        const auto finish = slot(kFinishNowSlot);
        const auto update = slot(kUpdateTeleportSlot);
        const auto query  = slot(kTeleportQuerySlot);
        g_variants[i].fact = &fact;
        if (!vtable || !is(force, fact.force) || !is(finish, fact.finish) || !is(update, fact.update) ||
            !is(query, fact.query))
        {
            platform::Log(L"party warp: the {} follow variant's slots do not check out; not driven", fact.name);
            continue;
        }
        g_variants[i].vtable = *vtable;
        g_variants[i].force  = reinterpret_cast<ForceFn>(*force);
        g_variants[i].finish = reinterpret_cast<FinishFn>(*finish);
        g_variants[i].update = reinterpret_cast<UpdateFn>(*update);
        g_variants[i].query  = reinterpret_cast<UpdateFn>(*query);
        chocoboVariants += fact.subject == Subject::Chocobo ? 1 : 0;
    }
    const auto partyIdle = platform::FindFunction(image, kPartyIdle);
    const auto inBattle  = platform::FindFunction(image, facts::kIsInBattle);
    if (g_variants[0].vtable == 0 || !partyIdle || !inBattle)
    {
        platform::Log(L"party warp: UNAVAILABLE (a check failed); a stranded player 2 is not brought back");
        return false;
    }
    g_partyIdle = reinterpret_cast<TestFn>(*partyIdle);
    g_inBattle  = reinterpret_cast<TestFn>(*inBattle);
    g_imageBase = image.Base();
    g_installed = true;
    platform::Log(L"party warp: a stranded player 2 is brought back by the game's follow warp (on foot; riding: {} of 3 "
                  L"chocobo follow variants checked)",
                  chocoboVariants);
    return true;
}

bool Busy() noexcept
{
    return g_state != nullptr;
}

bool Start(void* pawn, Subject subject, bool attached, bool splitShown) noexcept
{
    if (!g_installed || pawn == nullptr || !generation::OnGameThread())
    {
        return false;
    }
    if (g_state != nullptr)
    {
        platform::Log(L"party warp: {} not started (a warp of {} is being driven)", Who(subject), Who(g_subject));
        return false;
    }
    const void*    ai      = nullptr;
    const Variant* variant = nullptr;
    void*          state   = FollowStateOf(pawn, subject, &variant, &ai);
    bool           idle = false, battle = true;
    // A forced request skips the state's own can-teleport checks, so the game's other conditions for a party
    // warp are checked here. Riding: the game warps a rider's ride body, not the rider (RE-p2-party-warp.md §4), so a
    // member on a ride is refused and her chocobo is warped instead.
    const wchar_t* refused = !splitShown                                      ? L"the split is not on screen"
                             : subject == Subject::Member && attached         ? L"she is attached to something (riding?)"
                             : state == nullptr                               ? L"its AI is not in a checked follow state"
                             : ByteAt(state, kStateForced) != 0               ? L"a forced warp is already pending"
                             : variant->fact->skipGate && ByteAt(ai, kAiSkipsUpdate) != 0
                                 ? L"its AI skips the teleport update"
                             : ByteAt(pawn, kCharacterWarping) != 0           ? L"a warp is already queued on it"
                             : !Test(g_inBattle, battle)                      ? L"the battle test FAULTED"
                             : battle                                         ? L"in battle"
                             : !Test(g_partyIdle, idle)                       ? L"the party test FAULTED"
                             : !idle                                          ? L"the party manager is busy"
                                                                              : nullptr;
    if (refused != nullptr)
    {
        const void* current = PointerAt(PointerAt(ai, kAiBrain), kBrainState);
        platform::Log(L"party warp: {} not started ({}); its AI {} state vtable {:#x} type {}", Who(subject), refused, ai,
                      IdaVtableOf(current), ByteAt(current, kStateType));
        return false;
    }
    const FollowTeleportParams params{};
    if (!platform::CallGuarded(variant->force, state, &params))
    {
        platform::Log(L"party warp: {}'s forced request FAULTED; not driven", Who(subject));
        return false;
    }
    g_state   = state;
    g_variant = variant;
    g_pawn    = pawn;
    g_subject = subject;
    g_elapsed = 0.0f;
    g_updates = 0;
    platform::Log(L"party warp: forced follow warp requested for {} ({} follow state, mode 0, fade), pending {}; its AI "
                  L"skip flag {}",
                  Who(subject), variant->fact->name, ByteAt(state, kStateForced), ByteAt(ai, kAiSkipsUpdate));
    return true;
}

void Tick(void* member, const void* chocobo, bool splitShown, float seconds) noexcept
{
    if (g_state == nullptr || !generation::OnGameThread())
    {
        return;
    }
    const void* pawn = g_subject == Subject::Member ? member : chocobo;
    if (pawn != g_pawn || member == nullptr)
    {
        // Player 2 left, changed members or got off: the AI takes the pawn back and its own tick finishes the request.
        platform::Log(L"party warp: {} changed mid-warp after {:.2f} s ({}); left to its AI", Who(g_subject), g_elapsed,
                      g_subject == Subject::Chocobo && member != nullptr ? L"player 2 got off" : L"member changed or left");
        Stop();
        return;
    }
    if (FollowStateOf(pawn, g_subject) != g_state)
    {
        // The AI moved to another state under the same player. The follow state object stays in the brain
        // (it is the same one when it returns to following), so the game's finish-now still applies to it
        // and fades it back in, as long as it still is that state with the request pending.
        if (VariantOf(g_state, g_subject) == g_variant && ByteAt(g_state, kStateForced) != 0)
        {
            FinishNow(L"its AI left its follow state mid-warp");
        }
        else
        {
            platform::Log(L"party warp: {}'s AI left its follow state mid-warp after {:.2f} s; nothing pending",
                          Who(g_subject), g_elapsed);
            Stop();
        }
        return;
    }
    if (ByteAt(g_state, kStateForced) == 0)
    {
        platform::Log(L"party warp: {} done after {:.2f} s ({} update(s)), warp queued {}", Who(g_subject), g_elapsed,
                      g_updates, ByteAt(pawn, kCharacterWarping));
        Stop();
        return;
    }
    bool       battle = true;
    const bool tested = Test(g_inBattle, battle);
    if (!splitShown || !tested || battle)
    {
        FinishNow(!splitShown ? L"the split left the screen mid-warp"
                  : !tested   ? L"the battle test FAULTED mid-warp"
                              : L"a battle began mid-warp");
        return;
    }
    if (g_elapsed >= kFinishAfterSeconds)
    {
        FinishNow(L"not finished by the update");
        return;
    }
    // The two per-tick calls the gated AI would make: the query tick starts the follow EQS (with the pawn as
    // the requester) once the update has armed it; the update takes its result, fades, warps and commits.
    if (!platform::CallGuarded(g_variant->query, g_state, seconds) ||
        !platform::CallGuarded(g_variant->update, g_state, seconds))
    {
        platform::Log(L"party warp: {}'s teleport update FAULTED ({}); finishing", Who(g_subject),
                      platform::DescribeFault(platform::LastFault()).text);
        FinishNow(L"update fault");
        return;
    }
    ++g_updates;
    g_elapsed += seconds;
}

}  // namespace lss::player_two::party_warp
