#include "traversal/chocobo_climb.h"

#include "facts/engine.h"
#include "facts/move_state.h"
#include "platform/build_value.h"
#include "platform/dev_switch.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "rides/chocobo_ride.h"
#include "scopes/leader_scope.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::traversal::chocobo_climb {

namespace {

// 1.004 addresses (1.005 through replaced-chocobo-climb.json and the generated table; every one read in both exes).
constexpr std::uintptr_t kTryBeginSlot = 0x146585878;  // AEndFieldActionChocoboClimbActor vt+0x740 (1.005 +0x748)
constexpr std::uintptr_t kTryBegin     = 0x14339B3F0;  // ChocoboClimbActor_TryBegin(actor, dt, pawn)
constexpr std::uintptr_t kEnterSlot    = 0x146585DA8;  // the climb executer's state table: row 1's enter
constexpr std::uintptr_t kEnter        = 0x14338A56C;  // ChocoboClimbExecuter_State1_Enter(executer): the camera setup
// The pass calls the try-begin with the frame time in xmm1 and the pawn in r8 (both builds' disassembly).
using TryBeginFn    = void (*)(void* actor, float dt, void* pawn);
using EnterFn       = void (*)(void* executer);
#if LSS_DEV
// The game's own answers, read for the log alone (dev tools).
constexpr std::uintptr_t kHolderState  = 0x14085FCB0;  // CharCtrlHolder_GetResolvedStateByte68(holder)
using IsRiddenFn    = bool (*)(const void* character);
using HolderStateFn = std::uint8_t (*)(const void* holder);
#endif

// Fields (research/RE-p2-chocobo-climb.md; both builds unless per build).
constexpr std::uint32_t kExecuterChocobo      = 0x74;   // FWeakObjectPtr: the climbing chocobo, as the camera setup reads it
constexpr std::uint32_t kControlHolder        = 0x638;  // the character's control holder (state 8: a field action)
constexpr std::uint32_t kFieldActionCharacter = 0x5A0;  // AEndPlayerController: the character its field actions test
// TWeakPtr<FEndChocoboCameraModule>: the object, then its reference controller (shared count at +8), as Setup reads it.
inline constexpr platform::PerBuild<std::uint32_t> kCameraModule{0x1A50, 0x1A30};
constexpr std::uint32_t kModuleCurrent = 0xB60 + 0x18;  // the module's state machine (+0xB60): current entry, id first
constexpr std::uint32_t kModulePending = 0xB60 + 0x20;  // ... the requested entry (EndStateMachine_RequestStateById)
constexpr int           kNone          = -1;

platform::VTableEntry g_trySlot;
platform::VTableEntry g_enterSlot;

// Game thread, by seat: a seat's chocobo's counts at its index; index 0 holds the other chocobos' camera setups.
struct Counts
{
    unsigned tryScoped = 0, tryRefused = 0, enterScoped = 0, enterRefused = 0, enterOthers = 0;
};
std::array<Counts, players::kMaxPlayers> g_counts{};

#if LSS_DEV
// The logging half (dev tools): the game's answers around a start check, the 10 s summary, the module's state changes.
constexpr auto kSummaryEvery = std::chrono::seconds(10);

IsRiddenFn                            g_isRidden    = nullptr;  // null: not logged
HolderStateFn                         g_holderState = nullptr;
platform::Every g_summary{kSummaryEvery};

struct TryNote
{
    const void* actor  = nullptr;
    bool        ridden = false;
    int         who    = kNone;
    int         state  = kNone;
    bool operator==(const TryNote&) const = default;
};
// By seat (index 0 unused): the last start check noted, the seat's chocobo's module and its last state (Debug on
// change).
TryNote     g_lastTry[players::kMaxPlayers]{};
bool        g_tryNoted[players::kMaxPlayers]{};
const void* g_moduleSeen[players::kMaxPlayers]{};
int         g_moduleState[players::kMaxPlayers] = {kNone, kNone, kNone};
#endif

/// The chocobo's ride camera module while its weak handle holds, or null.
const void* ModuleOf(const void* chocobo) noexcept
{
    const void* module     = ReadPointer(chocobo, kCameraModule);
    const void* controller = ReadPointer(chocobo, kCameraModule + 8);
    return module != nullptr && controller != nullptr && ReadField<std::int32_t>(controller, 8) > 0 ? module : nullptr;
}

/// The id of the module state machine's entry at `field` (current or pending), or kNone.
int StateAt(const void* module, std::uint32_t field) noexcept
{
    const void* entry = ReadPointer(module, field);
    return entry != nullptr ? static_cast<int>(ReadField<std::uint32_t>(entry, 0)) : kNone;
}

std::wstring StateText(int state)
{
    return state == kNone ? std::wstring(L"none") : std::to_wstring(state);
}

struct TryCall
{
    int        seat;  // whose chocobo
    TryBeginFn fn;
    void*      actor;
    float      dt;
    void*      pawn;
#if LSS_DEV
    TryNote    note{};
    int        after = kNone;
#endif
};

#if LSS_DEV
/// The chocobo's control state as the game resolves it, or kNone (logging only).
int ControlState(const void* chocobo) noexcept
{
    const void*  holder = ReadPointer(chocobo, kControlHolder);
    std::uint8_t state  = 0;
    if (holder == nullptr || g_holderState == nullptr || !platform::CallGuarded(g_holderState, state, holder))
    {
        return kNone;
    }
    return state;
}
#endif

void RunTry(void* context) noexcept
{
    auto& call = *static_cast<TryCall*>(context);
#if LSS_DEV
    const bool logs = platform::Enabled(platform::Level::Debug);
    if (logs)
    {
        // Logging only: the game's own answers, read inside the seat's scope.
        call.note.actor = call.actor;
        if (g_isRidden == nullptr || !platform::CallGuarded(g_isRidden, call.note.ridden, static_cast<const void*>(call.pawn)))
        {
            call.note.ridden = false;
        }
        const void* character = ReadPointer(players::generation::ControllerOf(call.seat), kFieldActionCharacter);
        call.note.who         = character == nullptr                          ? 0
                              : character == players::generation::MemberOf(call.seat)  ? 1
                              : character == call.pawn                        ? 2
                                                                              : 3;
        call.note.state = ControlState(call.pawn);
    }
#endif
    call.fn(call.actor, call.dt, call.pawn);
#if LSS_DEV
    if (logs)
    {
        call.after = ControlState(call.pawn);
    }
#endif
}

#if LSS_DEV
void NoteTry(const TryCall& call)
{
    const int            seat = call.seat;
    const wchar_t* const name = players::SeatName(seat);
    platform::LogTrace(L"chocobo climb: {}'s chocobo's start check ran in {}'s scope", name, name);
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    if (!g_tryNoted[seat] || !(call.note == g_lastTry[seat]))
    {
        g_tryNoted[seat] = true;
        g_lastTry[seat]  = call.note;
        const std::wstring who[] = {L"none", std::format(L"{}'s member", name), std::format(L"{}'s chocobo", name),
                                    L"another character"};
        platform::LogDebug(L"chocobo climb: {}'s chocobo near {}: the game's start check runs in {}'s scope "
                           L"(the ridden vehicle {}; {}'s controller's field-action character: {}; the chocobo's "
                           L"control state {})",
                           name, players::NameOf(call.actor), name, call.note.ridden ? L"yes" : L"NO", name,
                           who[call.note.who < 0 ? 0 : call.note.who], StateText(call.note.state));
    }
    if (call.after != call.note.state && call.after != kNone)
    {
        platform::LogDebug(L"chocobo climb: {}'s chocobo starts a climb at {} (its control state {} -> {})", name,
                           players::NameOf(call.actor), StateText(call.note.state), StateText(call.after));
    }
}
#else
void NoteTry(const TryCall&) {}
#endif

/// The climb spot's try-begin (every climb spot on a controller's list, each frame, from the character's notify pass).
void TryBegin(void* actor, float dt, void* pawn) noexcept
{
    const auto original = reinterpret_cast<TryBeginFn>(g_trySlot.Original());
    // The seat from the chocobo it rides.
    const int seat = pawn != nullptr && players::generation::OnGameThread() ? rides::chocobo_ride::SeatRiding(pawn) : -1;
    if (seat >= 1)
    {
        TryCall call{seat, original, actor, dt, pawn};
        if (rides::chocobo_ride::RunAsRider(pawn, &RunTry, &call))
        {
            ++g_counts[seat].tryScoped;
            try
            {
                NoteTry(call);
            }
            catch (...)
            {
            }
            return;
        }
        ++g_counts[seat].tryRefused;
        platform::LogTrace(L"chocobo climb: {}'s chocobo's start check ran outside {}'s scope (refused)",
                           players::SeatName(seat), players::SeatName(seat));
    }
    original(actor, dt, pawn);
}

struct EnterCall
{
    int     seat;  // whose chocobo
    EnterFn fn;
    void*   executer;
    bool    leader = false;
};

void RunEnter(void* context) noexcept
{
    auto&                           call = *static_cast<EnterCall*>(context);
    const scopes::leader_scope::LeaderScope leader(scopes::leader_scope::WeakOf(players::generation::MemberOf(call.seat)));
    call.leader = leader.Held();
    call.fn(call.executer);
}

/// The climb executer's state 1 enter (the climb camera's setup), for every climbing chocobo.
void Enter(void* executer) noexcept
{
    const auto original = reinterpret_cast<EnterFn>(g_enterSlot.Original());
    if (executer == nullptr || !players::generation::OnGameThread())
    {
        original(executer);
        return;
    }
    const void* chocobo = scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(executer) + kExecuterChocobo);
    const int   seat    = rides::chocobo_ride::SeatRiding(chocobo);  // the seat from the climbing chocobo, or -1
    const bool  isSeat  = seat >= 1;
    const void* module  = chocobo != nullptr ? ModuleOf(chocobo) : nullptr;
    const int   state   = StateAt(module, kModuleCurrent);
    const int   pending = StateAt(module, kModulePending);
    EnterCall   call{seat, original, executer};
    const bool  scoped = isSeat && rides::chocobo_ride::RunAsRider(chocobo, &RunEnter, &call);
    if (!scoped)
    {
        original(executer);
    }
    ++(isSeat ? (scoped ? g_counts[seat].enterScoped : g_counts[seat].enterRefused) : g_counts[0].enterOthers);
    try
    {
        // The module is read again: the setup reads the chocobo's handle, it does not replace it [ASSUMPTION; logged].
        const void* after = chocobo != nullptr ? ModuleOf(chocobo) : nullptr;
        const auto  text  = std::format(L"its ride camera module {} state {} (pending {}) -> {} (pending {}; 1 start, 2 start "
                                        L"down)",
                                        after, StateText(state), StateText(pending), StateText(StateAt(after, kModuleCurrent)),
                                        StateText(StateAt(after, kModulePending)));
        if (isSeat && !scoped)
        {
            platform::Log(L"WARNING: chocobo climb: {}'s chocobo's climb camera setup ran outside {}'s scope (a "
                          L"scope was refused); no climb framing, as the game gives any rider but the leader; {}",
                          players::SeatName(seat), players::SeatName(seat), text);
        }
#if LSS_DEV
        else if (isSeat)
        {
            platform::LogDebug(L"chocobo climb: {}'s chocobo climbs; the camera setup ran in {}'s scope ({}'s member as "
                               L"the leader: {}); {}",
                               players::SeatName(seat), players::SeatName(seat), players::SeatName(seat),
                               call.leader ? L"yes" : L"REFUSED", text);
        }
        else
        {
            const void* controller = chocobo != nullptr ? players::actor_fields::Controller(chocobo) : nullptr;
            platform::LogDebug(L"chocobo climb: {} chocobo climbs; the camera setup ran as the game runs it; {}",
                               controller != nullptr && controller == players::generation::PlayerOneController() ? L"player 1's"
                                                                                                        : L"another",
                               text);
        }
#endif
    }
    catch (...)
    {
    }
}

#if LSS_DEV
void LogSummary() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    const auto     all    = std::exchange(g_counts, {});
    const unsigned others = all[0].enterOthers;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)  // a line per seat, each with the other chocobos' count
    {
        const Counts&        counts = all[seat];
        const wchar_t* const name   = players::SeatName(seat);
        if (counts.tryScoped + counts.tryRefused + counts.enterScoped + counts.enterRefused + others == 0)
        {
            continue;
        }
        platform::LogDebug(L"chocobo climb ({} s): {}'s chocobo's start checks in {}'s scope {} (refused {}); "
                           L"climb camera setups: {}'s in scope {} (refused {}), other chocobos' {}",
                           kSummaryEvery.count(), name, name, counts.tryScoped, counts.tryRefused, name,
                           counts.enterScoped, counts.enterRefused, others);
    }
}

/// Finds the game's answers the log reads; what it says in the install line.
const wchar_t* InstallLogging(const platform::GameImage& image) noexcept
{
    const auto isRidden = image.Rebase(facts::kIsActiveRiddenVehicle.address);
    const auto holder   = image.Rebase(kHolderState);
    g_isRidden          = isRidden ? reinterpret_cast<IsRiddenFn>(*isRidden) : nullptr;
    g_holderState       = holder ? reinterpret_cast<HolderStateFn>(*holder) : nullptr;
    return g_isRidden != nullptr && g_holderState != nullptr ? L"complete" : L"without the game's answers";
}
#else
void           LogSummary() noexcept {}
const wchar_t* InstallLogging(const platform::GameImage&) noexcept { return L"off (dev tools)"; }
#endif

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto trySlot   = image.Rebase(kTryBeginSlot, sizeof(void*));
    const auto tryFn     = image.Rebase(kTryBegin);
    const auto enterSlot = image.Rebase(kEnterSlot, sizeof(void*));
    const auto enterFn   = image.Rebase(kEnter);
    const wchar_t* logging = InstallLogging(image);
    // Both or neither: a climb without its camera would leave a seat's ride framing against the wall.
    const bool enter = enterSlot && enterFn &&
                       g_enterSlot.Replace(*enterSlot, *enterFn, reinterpret_cast<std::uintptr_t>(&Enter));
    const bool start = enter && trySlot && tryFn &&
                       g_trySlot.Replace(*trySlot, *tryFn, reinterpret_cast<std::uintptr_t>(&TryBegin));
    if (enter && !start)
    {
        g_enterSlot.Restore();
    }
    platform::Log(L"chocobo climb: {}; logging {}",
                  start ? L"player 2's or 3's chocobo starts its climbs and its climb camera in that player's scope (the spot's "
                          L"start check and the climb executer's camera setup)"
                        : L"NOT placed (a slot differs); player 2's or 3's chocobo cannot climb, as before",
                  logging);
}

void Remove() noexcept
{
    if (g_trySlot.Original() != 0)
    {
        g_trySlot.Restore();
    }
    if (g_enterSlot.Original() != 0)
    {
        g_enterSlot.Restore();
    }
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread() || g_enterSlot.Original() == 0)
    {
        return;
    }
    LogSummary();
#if LSS_DEV
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* chocobo = players::generation::Active(seat) ? rides::chocobo_ride::DrivenChocobo(seat) : nullptr;
        const void* module  = chocobo != nullptr ? ModuleOf(chocobo) : nullptr;
        const int   state   = StateAt(module, kModuleCurrent);
        if (module != g_moduleSeen[seat])
        {
            g_moduleSeen[seat]  = module;
            g_moduleState[seat] = state;
            continue;
        }
        if (module != nullptr && state != g_moduleState[seat])
        {
            const int was       = g_moduleState[seat];
            g_moduleState[seat] = state;
            try
            {
                platform::LogDebug(L"chocobo climb: {}'s ride camera module {} state {} -> {} (0 normal, 1 start, 2 "
                                   L"start down, 3 climb, 4 end climb, 5 swim)",
                                   players::SeatName(seat), module, StateText(was), StateText(state));
            }
            catch (...)
            {
            }
        }
    }
#endif
}

}  // namespace lss::traversal::chocobo_climb
