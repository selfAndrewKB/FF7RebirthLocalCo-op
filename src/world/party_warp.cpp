#include "world/party_warp.h"

#include "facts/controller.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "views/whole_screen.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::world::party_warp {

namespace {

// The follow-state variants whose four warp slots hold the functions the research read (research/RE-p2-party-warp.md for
// PLAYER; RE-p2-chocobo-warp.md for the chocobo sub-types). Each is checked at install, and a state is
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
    {L"PLAYER", Subject::Member, facts::kFollowStatePlayerVTable, 0x14119AD20, 0x14183CD64, 0x142ED7394, 0x140E22A50,
     true},
    {L"CHOCOBO_MEMBER", Subject::Chocobo, 0x145B1E1A0, 0x14119AD20, 0x14183CD64, 0x142ED7394, 0x142EDFAE8, false},
    {L"CHOCOBO_SUB", Subject::Chocobo, 0x145B1D230, 0x14119AD20, 0x14183CD64, 0x142ED7394, 0x142EDFAE8, false},
    {L"CHOCOBO", Subject::Chocobo, 0x145B1E488, 0x142ED1118, 0x14183CD64, 0x142ED7394, 0x142EDFAC0, false},
};
constexpr std::size_t kVariantCount = sizeof(kVariants) / sizeof(kVariants[0]);

// PartyManager_IsIdle (bool()), the guard PartyFollow_ForceTeleportAll uses before it forces anyone.
constexpr std::uint8_t kPartyIdleBytes[] = {0x48, 0x8B, 0x05, 0x81, 0xBD, 0x12, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x07,
                                            0x80, 0x38, 0x00, 0x0F, 0x94, 0xC0, 0xC3, 0xB0, 0x01, 0xC3, 0xCC, 0xCC};
constexpr platform::FunctionFact kPartyIdle{L"PartyManager_IsIdle", 0x1410AC788, kPartyIdleBytes};

// The game's "this follower must not teleport now" setter, its lifts' (research/RE-chocobo-summon-gather.md): it writes
// the pawn's current AI state's no-teleport flag only while the pawn's controller is its AI in FOLLOW.
constexpr std::uint8_t kNoTeleportBytes[] = {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xE3, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C,
                                             0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x41, 0x56, 0xB8, 0x80, 0x00};
constexpr platform::FunctionFact kNoTeleport{L"Pawn_SetFollowStateNoTeleport6CC_IfFollow", 0x1414B9ED4,
                                             kNoTeleportBytes};
using facts::kFollowStateNoTeleport;

// The follow update's own gates a forced request skips (research/RE-warp-gates.md):
//   G1: the character's field controller state, its virtual check (ladder, squeeze, crawl, climbing wall, grappling
//       hook, jump spot and the rest): the game asks it of the leader and the follow target; the mod of both player 1's
//       character and the one it brings back (a held member keeps a FOLLOW state while its player steers it). The slot
//       of EndPlayerCharacter's vtable is checked at install; a character answers only through that function or the
//       one it calls (the two the research found in every party member's vtable).
//   G2: the field-exclusion mask (a scene, a dialog, a minigame holds the field), read by views/whole_screen.
//   G4: the field step (13 once the field is up); a forced request waits on it, the mod's finish-now would not.
constexpr platform::PerBuild<std::uint32_t> kFieldStateSlot = {0x938, 0x930};
constexpr std::uintptr_t kEndPlayerCharacterVTable = 0x1456B3198;
constexpr std::uintptr_t kFieldStateCheck          = 0x1409A5A84;  // the slot's function
constexpr std::uint8_t   kFieldStateBytes[]        = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kFieldState{L"EndCharacter_IsInFieldControllerState", 0x141985068, kFieldStateBytes};
constexpr std::uint32_t kFieldStep        = 0x70;  // EEndFieldStep
constexpr std::uint8_t  kFieldStepUp      = 13;
constexpr std::uint32_t kAiSkipsUpdate    = 0xAD8;  // the PLAYER update returns at once while set
constexpr std::uint32_t kCharacterWarping = 0xB59;  // a warp request is queued on the character
// The AI's active brain, its state and the follow state's forced request: facts/party.h.
using facts::kAiActiveBrain;
using facts::kAiStateFollow;
using facts::kAiStateType;
using facts::kFollowStateForced;
// The teleport query runner (research/RE-p2-party-warp.md): the result pointer and its status (1 = done).
constexpr std::uint32_t kStateQueryResult = 0x540;
constexpr std::uint32_t kQueryStatus      = 0x28;

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

using FieldStateFn = bool (*)(const void* character);
using ForceFn  = void (*)(void* state, const FollowTeleportParams* params);
using UpdateFn = void (*)(void* state, float seconds);  // also the query slot's type
using FinishFn = void (*)(void* state);
using TestFn   = bool (*)();
using NoTeleportFn = void (*)(void* pawn, bool on);

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
NoTeleportFn   g_noTeleport = nullptr;  // optional: without it no follower is held off the game's warp
std::uintptr_t g_fieldStateCheck = 0;  // G1's two live functions (0: not checked, the gate refuses every warp)
std::uintptr_t g_fieldState      = 0;
std::uintptr_t g_gameRoot        = 0;  // the address holding the game root
std::optional<platform::GameImage> g_image;  // for research IDA addresses in the log
bool           g_installed = false;

// Game thread. One warp at a time, owned by a seat: a second seat's Start is refused while it runs, and nav_strand
// holds that seat's request until the warp is over (the two-player order of calls kept; whether the game takes two
// forced follow warps at once is not known).
void*          g_state   = nullptr;  // the follow state being driven
const Variant* g_variant = nullptr;  // its variant
const void*    g_pawn    = nullptr;  // whose state it is (the member, or the chocobo)
int            g_seat    = -1;       // whose warp it is (players/seats.h)
Subject        g_subject = Subject::Member;
float          g_elapsed = 0.0f;
int            g_updates = 0;

/// A byte of `object`; 0xFF when unreadable, so a state that cannot be read is neither FOLLOW nor "nothing
/// pending".
std::uint8_t ByteAt(const void* object, std::uint32_t offset) noexcept
{
    std::uint8_t value = 0xFF;
    Read(object, offset, value);
    return value;
}

const wchar_t* Who(int seat, Subject subject) noexcept
{
    constexpr const wchar_t* kMembers[players::kMaxPlayers]  = {L"player 1's member", L"player 2's member",
                                                                L"player 3's member"};
    constexpr const wchar_t* kChocobos[players::kMaxPlayers] = {L"player 1's chocobo", L"player 2's chocobo",
                                                                L"player 3's chocobo"};
    if (seat < 0 || seat >= players::kMaxPlayers)
    {
        return subject == Subject::Member ? L"a player's member" : L"a player's chocobo";
    }
    return subject == Subject::Member ? kMembers[seat] : kChocobos[seat];
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
void* FollowOf(const void* pawn, Subject subject, const Variant** variantOut = nullptr,
               const void** aiOut = nullptr) noexcept
{
    void* ai    = players::actor_fields::RetainedAi(pawn);
    void* state = players::actor_fields::BrainState(players::actor_fields::ActiveBrain(ai));
    if (aiOut != nullptr)
    {
        *aiOut = ai;
    }
    const Variant* variant = ByteAt(state, kAiStateType) == kAiStateFollow ? VariantOf(state, subject) : nullptr;
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
    return Read(object, 0, vtable) && g_image ? g_image->SourceAddress(vtable) : 0;
}

bool Test(TestFn fn, bool& out) noexcept
{
    return platform::CallGuarded(fn, out);
}

// Each gate below is skipped when its install check failed (a warning at install): the warp then works as before the
// gate. A gate whose check runs but cannot answer refuses.

/// G1 on `pawn`: true when it is a party character in a field action or another field controller state, or when its
/// check faults. A pawn whose slot holds neither checked function is no party character (a mount, a vehicle: the game
/// asks the character, never its ride) and answers false.
bool FieldActionBusy(const void* pawn) noexcept
{
    std::uintptr_t vtable = 0, slot = 0;
    if (pawn == nullptr || g_fieldStateCheck == 0 || !Read(pawn, 0, vtable) ||
        !Read(reinterpret_cast<const void*>(vtable), static_cast<std::uint32_t>(kFieldStateSlot), slot) ||
        (slot != g_fieldStateCheck && slot != g_fieldState))
    {
        return false;
    }
    bool busy = true;
    return !platform::CallGuarded(reinterpret_cast<FieldStateFn>(slot), busy, pawn) || busy;
}

/// G4: the field is up (its step has reached 13).
bool FieldUp() noexcept
{
    const void*  root = nullptr;
    std::uint8_t step = 0;
    return g_gameRoot == 0 || (Read(reinterpret_cast<const void*>(g_gameRoot), root) && root != nullptr &&
                               Read(root, kFieldStep, step) && step >= kFieldStepUp);
}

/// G2: a scene, a dialog or a minigame holds the field (its exclusion mask, as views/whole_screen reads it), or the
/// game state cannot be read now.
bool FieldHeld() noexcept
{
    if (!views::WholeScreenInstalled())
    {
        return false;
    }
    const views::ScreenSignals signals = views::ReadScreenSignals();
    return !signals.readable || signals.exclusion != 0;
}

void Stop() noexcept
{
    g_state   = nullptr;
    g_variant = nullptr;
    g_pawn    = nullptr;
    g_seat    = -1;
    g_elapsed = 0.0f;
    g_updates = 0;
}

/// The follow query's status for a log line: 1 = a result is ready; -1 = no result object (the query never ran).
int QueryStatus() noexcept
{
    const void* result = ReadPointer(g_state, kStateQueryResult);
    std::uint8_t status = 0;
    return result == nullptr ? -1 : Read(result, kQueryStatus, status) ? status : -2;
}

/// The game's own completion of a forced request (warp to the follow target, fade in).
void FinishNow(const wchar_t* why) noexcept
{
    const int  query = QueryStatus();
    const bool ok    = platform::CallGuarded(g_variant->finish, g_state);
    platform::Log(L"party warp: {} ({}) {} after {:.2f} s ({} update(s), query status {}); the game's finish-now called{}",
                  Who(g_seat, g_subject), g_variant->fact->name, why, g_elapsed, g_updates, query,
                  ok ? L"" : L" and FAULTED");
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
        platform::Log(L"party warp: UNAVAILABLE (a check failed); a stranded {} is not brought back",
                      players::kSeatsBuilt == 1 ? L"player 2" : L"player 2 or 3");
        return false;
    }
    g_partyIdle = reinterpret_cast<TestFn>(*partyIdle);
    g_inBattle  = reinterpret_cast<TestFn>(*inBattle);
    const auto noTeleport = platform::FindFunction(image, kNoTeleport);
    g_noTeleport          = noTeleport ? reinterpret_cast<NoTeleportFn>(*noTeleport) : nullptr;
    if (g_noTeleport == nullptr)
    {
        platform::LogWarning(L"party warp: the game's no-teleport setter was not found; the follow AI of a chocobo the "
                             L"game seats player 2 or 3 on may still warp it to player 1 before they take it");
    }
    // G1: the base character vtable's slot holds the checked function; the function it calls is the other answer.
    const auto characterVTable = image.Rebase(kEndPlayerCharacterVTable, sizeof(void*));
    const auto fieldState      = platform::FindFunction(image, kFieldState);
    std::uintptr_t slot        = 0;
    if (characterVTable && fieldState &&
        Read(reinterpret_cast<const void*>(*characterVTable), static_cast<std::uint32_t>(kFieldStateSlot), slot) &&
        is(std::optional<std::uintptr_t>{slot}, kFieldStateCheck))
    {
        g_fieldStateCheck = slot;
        g_fieldState      = *fieldState;
    }
    const auto gameRoot = players::game_globals::Address(players::game_globals::Global::GameRoot);
    g_gameRoot          = gameRoot;
    g_image = image;
    g_installed = true;
    platform::Log(L"party warp: a stranded {} is brought back by the game's follow warp (on foot; riding: {} of 3 "
                  L"chocobo follow variants checked)",
                  players::kSeatsBuilt == 1 ? L"player 2" : L"player 2 or 3", chocoboVariants);
    if (g_fieldStateCheck == 0 || g_gameRoot == 0 || !views::WholeScreenInstalled())
    {
        platform::LogWarning(L"party warp: a check of the follow warp's own gates failed (the field action {}, the field "
                             L"step {}, the scene mask {}); the warp runs without that gate",
                             g_fieldStateCheck != 0 ? L"ok" : L"FAILED", g_gameRoot != 0 ? L"ok" : L"FAILED",
                             views::WholeScreenInstalled() ? L"ok" : L"FAILED");
    }
    return true;
}

bool Busy() noexcept
{
    return g_state != nullptr;
}

bool InFieldAction(const void* pawn) noexcept
{
    return FieldActionBusy(pawn);
}

int BusySeat() noexcept
{
    return g_state != nullptr ? g_seat : -1;
}

std::wstring AiText(const void* pawn, Subject subject) noexcept
{
    const void*    ai      = nullptr;
    const Variant* variant = nullptr;
    const void*    state   = FollowOf(pawn, subject, &variant, &ai);
    const void*    current = players::actor_fields::BrainState(players::actor_fields::ActiveBrain(ai));
    std::uint32_t  pause   = 0;
    float          range   = 0.0f;
    Read(ai, facts::kAiPauseMask, pause);
    Read(current, facts::kFollowStateTeleportRange, range);
    // Without the animation budget's bit, which flips every frame on a throttled mesh (facts::kAiPauseAnimationBudget):
    // the strand probe compares this text, and that bit is no change in the character's state.
    pause &= ~facts::kAiPauseAnimationBudget;
    try
    {
        const std::wstring head = std::format(L"AI {} (pause mask {:#x}), state {} type {}", ai, pause, current,
                                              ByteAt(current, kAiStateType));
        return state != nullptr ? head + std::format(L" ({} follow: teleport range {:.0f}, forced warp pending {})",
                                                     variant->fact->name, range, ByteAt(state, kFollowStateForced))
                                : head + std::format(L" (vtable {:#x}, not a checked follow state)",
                                                     IdaVtableOf(current));
    }
    catch (...)
    {
        return L"AI ?";
    }
}

bool CanHoldOffWarp() noexcept
{
    return g_noTeleport != nullptr;
}

std::optional<bool> WarpHeldOff(const void* pawn, Subject subject) noexcept
{
    const void* state = FollowOf(pawn, subject);
    if (state == nullptr)
    {
        return std::nullopt;
    }
    const std::uint8_t held = ByteAt(state, kFollowStateNoTeleport);
    return held <= 1 ? std::optional<bool>(held != 0) : std::nullopt;
}

std::optional<bool> HoldOffWarp(void* pawn, Subject subject, bool on) noexcept
{
    if (g_noTeleport == nullptr || pawn == nullptr || !players::generation::OnGameThread() ||
        !platform::CallGuarded(g_noTeleport, pawn, on))
    {
        return std::nullopt;
    }
    return WarpHeldOff(pawn, subject);
}

bool Start(int seat, void* pawn, Subject subject, bool attached, bool splitShown) noexcept
{
    if (!g_installed || pawn == nullptr || !players::SeatBuilt(seat) || !players::generation::OnGameThread())
    {
        if (!g_installed && players::SeatBuilt(seat))
        {
            platform::Log(L"party warp: {} not started (the warp is not installed, see the startup log)",
                          Who(seat, subject));
        }
        return false;
    }
    if (g_state != nullptr)
    {
        platform::Log(L"party warp: {} not started (a warp of {} is being driven)", Who(seat, subject),
                      Who(g_seat, g_subject));
        return false;
    }
    const void*    ai      = nullptr;
    const Variant* variant = nullptr;
    void*          state   = FollowOf(pawn, subject, &variant, &ai);
    bool           idle = false, battle = true;
    // A forced request skips the state's own can-teleport checks, so the game's other conditions for a party
    // warp are checked here. Riding: the game warps a rider's ride body, not the rider (research/RE-p2-party-warp.md), so a
    // member on a ride is refused and her chocobo is warped instead.
    // The follow update's own gates first (RE-warp-gates.md), then the forced request's. G1 asks player 1's pawn (no
    // answer from a mount or vehicle) and the member on foot; a chocobo's warp carries its seated rider.
    const void*    leader  = players::actor_fields::Pawn(players::generation::PlayerOneController());
    const wchar_t* refused = !splitShown                                      ? L"the split is not on screen"
                             : !FieldUp()                                     ? L"the field is not up"
                             : FieldHeld()                                    ? L"a scene, a talk or an event holds the field"
                             : FieldActionBusy(leader)                          ? L"player 1's character is in a field action"
                             : subject == Subject::Member && FieldActionBusy(pawn) ? L"it is in a field action"
                             : subject == Subject::Member && attached         ? L"she is attached to something (riding?)"
                             : state == nullptr                               ? L"its AI is not in a checked follow state"
                             : ByteAt(state, kFollowStateForced) != 0         ? L"a forced warp is already pending"
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
        platform::Log(L"party warp: {} not started ({}); its {}", Who(seat, subject), refused, AiText(pawn, subject));
        return false;
    }
    if (subject == Subject::Chocobo && ByteAt(state, kFollowStateNoTeleport) == 1)
    {
        // The no-teleport hold cancels even a forced request at each update: on a chocobo it is the mod's own (the
        // game's lifts hold characters), so the mod's own warp lifts it.
        const auto held = HoldOffWarp(pawn, subject, false);
        platform::Log(L"party warp: {}'s no-teleport hold lifted for this warp ({})", Who(seat, subject),
                      held == false  ? L"cleared"
                      : held == true ? L"could NOT be cleared; the request may be cancelled"
                                     : L"not in FOLLOW or no setter; the request may be cancelled");
    }
    const FollowTeleportParams params{};
    if (!platform::CallGuarded(variant->force, state, &params))
    {
        platform::Log(L"party warp: {}'s forced request FAULTED; not driven", Who(seat, subject));
        return false;
    }
    g_state   = state;
    g_variant = variant;
    g_pawn    = pawn;
    g_seat    = seat;
    g_subject = subject;
    g_elapsed = 0.0f;
    g_updates = 0;
    platform::Log(L"party warp: forced follow warp requested for {} ({} follow state, mode 0, fade), pending {}; its AI "
                  L"skip flag {}",
                  Who(seat, subject), variant->fact->name, ByteAt(state, kFollowStateForced), ByteAt(ai, kAiSkipsUpdate));
    return true;
}

void Tick(const SeatPawns& members, const SeatPawns& chocobos, bool splitShown, float seconds) noexcept
{
    if (g_state == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    const void* member = members[static_cast<std::size_t>(g_seat)];
    const void* pawn   = g_subject == Subject::Member ? member : chocobos[static_cast<std::size_t>(g_seat)];
    if (pawn != g_pawn || member == nullptr)
    {
        // The seat's player left, changed members or got off: the AI takes the pawn back and its own tick finishes the
        // request.
        constexpr const wchar_t* kGotOff[players::kMaxPlayers] = {L"player 1 got off", L"player 2 got off",
                                                                  L"player 3 got off"};
        platform::Log(L"party warp: {} changed mid-warp after {:.2f} s ({}); left to its AI", Who(g_seat, g_subject),
                      g_elapsed,
                      g_subject == Subject::Chocobo && member != nullptr ? kGotOff[g_seat] : L"member changed or left");
        Stop();
        return;
    }
    if (FollowOf(pawn, g_subject) != g_state)
    {
        // The AI moved to another state under the same player. The follow state object stays in the brain
        // (it is the same one when it returns to following), so the game's finish-now still applies to it
        // and fades it back in, as long as it still is that state with the request pending.
        if (VariantOf(g_state, g_subject) == g_variant && ByteAt(g_state, kFollowStateForced) != 0)
        {
            FinishNow(L"its AI left its follow state mid-warp");
        }
        else
        {
            platform::Log(L"party warp: {}'s AI left its follow state mid-warp after {:.2f} s; nothing pending",
                          Who(g_seat, g_subject), g_elapsed);
            Stop();
        }
        return;
    }
    if (ByteAt(g_state, kFollowStateForced) == 0)
    {
        platform::Log(L"party warp: {} done after {:.2f} s ({} update(s)), warp queued {}", Who(g_seat, g_subject), g_elapsed,
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
        platform::Log(L"party warp: {}'s teleport update FAULTED ({}); finishing", Who(g_seat, g_subject),
                      platform::DescribeFault(platform::LastFault()).text);
        FinishNow(L"update fault");
        return;
    }
    ++g_updates;
    g_elapsed += seconds;
}

}  // namespace lss::world::party_warp
