#include "player_two/member_movement.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "platform/every.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/battle_camera.h"
#include "player_two/battle_input.h"
#include "player_two/camera_override.h"
#include "player_two/chocobo_ride.h"
#include "player_two/climb_context.h"
#include "player_two/lock_on.h"
#include "player_two/generation.h"
#include "player_two/move_state.h"
#include "player_two/object_fields.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"
#include "player_two/scope_leaks.h"
#include "player_two/wheelie_ride.h"

#include <atomic>
#include <cstddef>

namespace lss::player_two::member_movement {

namespace {

using TickForCharacterFn = void (*)(void* moveState, float deltaTime);
using LeaderTestFn       = std::uint8_t (*)(const void* component);
using PlayerTypeFn       = std::uint8_t (*)(const void* component);
using HandleClearFn      = void (*)(void* handle);

constexpr int kNoType = -1;

// Set once at install.
platform::VTableEntry g_leaderSlot;
TickForCharacterFn    g_tickForCharacter = nullptr;
LeaderTestFn          g_engineLeaderTest = nullptr;
HandleClearFn         g_handleClear      = nullptr;  // null: player 2's field-action rows are left to show
LeaderTestFn          g_isPlayerCore     = nullptr;  // IsUCPC_Core(character), for the ride logging only
LeaderTestFn          g_isRiddenVehicle  = nullptr;  // IsActiveRiddenVehicle(character), for the ride logging only
std::uintptr_t        g_moveStateVTable  = 0;

// The player type the leader test says yes to; kNoType outside the member's tick. Written on the
// game thread; the leader test is also called on other threads, which read kNoType.
std::atomic<int>      g_leaderType{kNoType};
// A second yes, by pointer: player 2's ridden chocobo's input component (every chocobo's has player type 16, player
// 1's included, so it cannot be answered by type; RE-p2-chocobo-ride.md §7.0 item 2). Null outside the ride's tick.
std::atomic<const void*> g_leaderComponent{nullptr};
std::atomic<unsigned> g_leaderAnswers{0};
// The player type the leader test says yes to on this thread only (ThreadLeaderAnswer), or kNoType.
thread_local int      t_threadType = kNoType;
// The only player type the leader test says yes to on this thread, every other type a no (ExchangeThreadOnlyLeader),
// or kNoType.
thread_local int      t_onlyType   = kNoType;

// The leader-test slot holds LeaderTest (LeaderTestReplaced); any thread reads it.
std::atomic<bool>     g_leaderReplaced{false};

// Game thread only: set once the engine raised an exception inside the member's tick.
bool g_stopped = false;

StageCounters g_counters;

int PlayerTypeOfComponent(const void* component) noexcept
{
    const auto vtable = reinterpret_cast<const void*>(VTableOf(component));
    const auto fn     = vtable == nullptr ? nullptr : ReadField<PlayerTypeFn>(vtable, facts::kPlayerTypeSlotOffset);
    return fn == nullptr ? kNoType : fn(component);  // an unreadable slot reads null
}

std::uint8_t LeaderTest(const void* component)
{
    // An enemy's target choice (ExchangeThreadOnlyLeader): exactly one player-controlled type, over every other answer. The
    // engine's own test compares the same type byte with the party manager's leader type (RE-p2-enemy-target-bias.md
    // §10.3), so this is that test with the chosen type as the leader's.
    if (t_onlyType != kNoType)
    {
        return PlayerTypeOfComponent(component) == t_onlyType ? 1 : 0;
    }
    // Player 2's climb on the anim worker (climb_context.h), or player 2's chocobo's actions there (ThreadLeaderAnswer):
    // this thread only, the member's type.
    const int climbType = climb_context::LeaderTypeHere();
    const int hereType  = climbType != kNoType ? climbType : t_threadType;
    if (hereType != kNoType && PlayerTypeOfComponent(component) == hereType)
    {
        g_leaderAnswers.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    const int   type   = g_leaderType.load(std::memory_order_relaxed);
    const void* extra  = g_leaderComponent.load(std::memory_order_relaxed);
    if ((type != kNoType || extra != nullptr) && generation::OnGameThread() &&
        ((extra != nullptr && component == extra) || (type != kNoType && PlayerTypeOfComponent(component) == type)))
    {
        g_leaderAnswers.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return g_engineLeaderTest(component);
}

/// Answers yes to the leader test for one player type (and one component, if given) while it lives.
class LeaderAnswer
{
public:
    explicit LeaderAnswer(int type, const void* component = nullptr) noexcept
        : savedType_(g_leaderType.exchange(type, std::memory_order_relaxed)),
          savedComponent_(g_leaderComponent.exchange(component, std::memory_order_relaxed))
    {
    }
    ~LeaderAnswer()  // what was answered before (none outside a scope): nesting keeps the outer answer
    {
        g_leaderType.store(savedType_, std::memory_order_relaxed);
        g_leaderComponent.store(savedComponent_, std::memory_order_relaxed);
    }
    LeaderAnswer(const LeaderAnswer&)            = delete;
    LeaderAnswer& operator=(const LeaderAnswer&) = delete;

private:
    int         savedType_;
    const void* savedComponent_;
};

int MemberType(const void* member) noexcept
{
    const auto type = party_member::PlayerTypeOf(member);
    return type ? *type : kNoType;
}

/// The engine's tick inside a structured-exception guard (platform/guarded.h).
bool GuardedTick(void* state, float deltaTime) noexcept
{
    __try
    {
        g_tickForCharacter(state, deltaTime);
        return true;
    }
    __except (platform::GuardFilterRecording(GetExceptionInformation()))
    {
        return false;
    }
}

/// The game's chocobo slot 1 inside a structured-exception guard.
bool GuardedHandles(HandlesFn engine, void* state, float deltaTime, std::uintptr_t& returned) noexcept
{
    __try
    {
        returned = engine(state, deltaTime);
        return true;
    }
    __except (platform::GuardFilterRecording(GetExceptionInformation()))
    {
        return false;
    }
}

/// The game's ridden-vehicle test for `chocobo` in the current context: 1, 0, or -1 when unavailable or it faulted.
std::int32_t RiddenTest(const void* chocobo) noexcept
{
    std::uint8_t answer = 0;
    return g_isRiddenVehicle != nullptr && platform::CallGuarded(g_isRiddenVehicle, answer, chocobo) ? answer : -1;
}

/// The game's handle clear inside a structured-exception guard.
bool GuardedClear(void* handle) noexcept
{
    __try
    {
        g_handleClear(handle);
        return true;
    }
    __except (platform::GuardFilterRecording(GetExceptionInformation()))
    {
        return false;
    }
}

/// Puts the state's field-action rows back to how the game leaves any character it did not tick.
/// The tick asks for the party's summon rows (chocobo, buggy, cactus statue) from whichever state it
/// runs, and the character's own tick would then add them to the one shared prompt list as rows of
/// their own (RE-p2-field-action-row.md). The requests are used only inside the tick, so clearing
/// them afterwards changes the rows alone.
void ClearFieldActionRows(void* state) noexcept
{
    if (g_handleClear == nullptr)
    {
        return;
    }
    for (const std::uint32_t offset : facts::kMoveStateFieldActionHandles)
    {
        if (!GuardedClear(static_cast<std::byte*>(state) + offset))
        {
            g_handleClear = nullptr;
            platform::Log(L"member movement: the engine raised an exception clearing player 2's field-action rows; "
                          L"they are left to show this session");
            return;
        }
    }
}

/// Ticks the member's state inside every scope, or not at all.
void TickInScopes(void* playerTwoController, void* member, void* state, int type, float deltaTime) noexcept
{
    const pad_sticks::StickScope    sticks(pad_sticks::ControllerIdOf(playerTwoController));
    const primary_controller::Scope primary;
    if (!sticks.Held() || !primary.Held())
    {
        g_counters.Refuse();
        return;
    }
    const pad_buttons::ButtonScope   buttons;  // player 2's own buttons (sprint, dodge, field actions)
    const lock_on::TargetScope       targets;  // in battle, the target system's fields player 2's own
    const LeaderAnswer               leader(type);
    const move_state::OwnTick        own;
    const scope_leaks::MemberTickScope leaks(state, playerTwoController, deltaTime);
    g_counters.Act();
    // Before the tick, inside every scope (player 2's camera manager is the primary one here):
    camera_override::Tick(member);         // the member's animation camera overrides reach player 2's camera
    lock_on::AfterBattle();                // out of battle, player 2's lock ends
    battle_camera::Watch();                // both camera managers' operators, logged on change (dev tools)
    battle_input::Watch(member, state);     // player 2's battle state, logged on change (dev tools)
    if (!GuardedTick(state, deltaTime))
    {
        g_stopped = true;
        return;
    }
    ClearFieldActionRows(state);
    wheelie_ride::AfterMemberTick(playerTwoController, member);  // a Wheelie walk-in the tick answered yes to: boarded here
}

/// The member's move state, if the member is player 2's possessed character and the tick's
/// unchecked reads hold (the controller's leader-action component).
void* MoveStateToTick(void* playerTwoController, void* member) noexcept
{
    void* state = ReadPointer(member, facts::kCharacterMoveState);
    if (VTableOf(state) != g_moveStateVTable || ReadPointer(state, facts::kMoveStateCharacter) != member ||
        ReadPointer(member, facts::kPawnController) != playerTwoController ||
        ReadPointer(playerTwoController, facts::kControllerLeaderActions) == nullptr)
    {
        return nullptr;
    }
    return state;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick       = platform::FindFunction(image, facts::kMoveStateTickForCharacter);
    const auto leaderTest = platform::FindFunction(image, facts::kIsPartyLeader.target);
    const auto vtable     = image.Rebase(facts::kMoveStateVTable);
    if (!tick || !leaderTest || !vtable || !party_member::Installed() || !primary_controller::Installed() ||
        !pad_sticks::Installed())
    {
        return false;
    }
    g_tickForCharacter = reinterpret_cast<TickForCharacterFn>(*tick);
    g_engineLeaderTest = reinterpret_cast<LeaderTestFn>(*leaderTest);
    g_moveStateVTable  = *vtable;
    const auto core    = platform::FindFunction(image, facts::kIsPlayerCore);
    g_isPlayerCore     = core ? reinterpret_cast<LeaderTestFn>(*core) : nullptr;
    const auto ridden  = platform::FindFunction(image, facts::kIsActiveRiddenVehicle);
    g_isRiddenVehicle  = ridden ? reinterpret_cast<LeaderTestFn>(*ridden) : nullptr;
    const auto clear   = platform::FindFunction(image, facts::kFieldActionHandleClear);
    g_handleClear      = clear ? reinterpret_cast<HandleClearFn>(*clear) : nullptr;
    if (!clear)
    {
        platform::Log(L"member movement: the field-action handle clear differs; player 2's member will add its own "
                      L"(dim) summon rows to the prompt list");
    }
    const bool replaced = platform::ReplaceSlot(image, facts::kIsPartyLeader, reinterpret_cast<std::uintptr_t>(&LeaderTest),
                                                g_leaderSlot);
    g_leaderReplaced.store(replaced, std::memory_order_release);
    return replaced;
}

void Remove() noexcept
{
    if (g_leaderSlot.Restore())
    {
        g_leaderReplaced.store(false, std::memory_order_release);
    }
    else
    {
        platform::Log(L"could not restore the leader-test slot; the replacement stays in place");
    }
}

void Tick(void* playerTwoController, void* member) noexcept
{
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::MemberTick);
    if (!generation::OnGameThread())
    {
        return;
    }
    const auto engine = move_state::TakeEngineTicks();
    if (!pad_sticks::Enabled() || !pad_sticks::PartActs(pad_sticks::kPartMemberTick) || !generation::Active() ||
        g_tickForCharacter == nullptr || g_stopped)
    {
        // A ride left un-ticked would hold player 2's controller on a chocobo nobody steers or ends: given back.
        chocobo_ride::Release(L"the member's tick stopped (per-pad sticks switched off, or the tick stopped)");
        wheelie_ride::Release(L"the member's tick stopped (per-pad sticks switched off, or the tick stopped)");
        return;
    }
    if (chocobo_ride::Tick(playerTwoController, member, engine))
    {
        return;  // player 2 drives their chocobo: its move state was ticked instead of the member's
    }
    if (wheelie_ride::Tick(playerTwoController, member, engine))
    {
        return;  // player 2 rides a Wheelie: its move state was ticked instead of the member's
    }
    // While player 1 rides a scooter, the field ticker runs only its ride body (no on-foot state is counted): that ride
    // tick is the frame's tick for player 2's member too (live 2026-10-06: she stood still while player 1 rode).
    if ((engine.count == 0 && !engine.rideTicked) || engine.memberTicked)
    {
        g_counters.Pass();  // the game's field ticker is idle (a menu, an event), or ran the member itself
        return;
    }
    void*     state = MoveStateToTick(playerTwoController, member);
    const int type  = MemberType(member);
    if (state == nullptr || type == kNoType)
    {
        g_counters.Refuse();
        return;
    }
    TickInScopes(playerTwoController, member, state, type, engine.count > 0 ? engine.deltaTime : engine.rideDeltaTime);
    if (g_stopped)
    {
        platform::Log(L"member movement STOPPED: the engine raised an exception inside the member's move-state tick; "
                      L"player 2's member is no longer ticked this session");
    }
}

StageCounters& Counters() noexcept
{
    return g_counters;
}

unsigned TakeLeaderAnswers() noexcept
{
    return platform::Take(g_leaderAnswers);
}

std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept
{
    // The same type, no component: the guard restores both, as every other answer scope does.
    const LeaderAnswer leader(g_leaderType.load(std::memory_order_relaxed));
    return test(character);
}

bool InPrimaryScope(const void* member, void (*call)(void* context), void* context) noexcept
{
    const int type = MemberType(member);
    if (!generation::OnGameThread() || type == kNoType)
    {
        return false;
    }
    const primary_controller::Scope primary;
    if (!primary.Held())
    {
        return false;
    }
    const LeaderAnswer leader(type);
    call(context);
    return true;
}

void ClearRideRows(void* chocoboState) noexcept
{
    if (g_handleClear == nullptr || chocoboState == nullptr || !generation::OnGameThread())
    {
        return;
    }
    for (const std::uint32_t offset : facts::kChocoboMoveStateRowHandles)
    {
        if (!GuardedClear(static_cast<std::byte*>(chocoboState) + offset))
        {
            g_handleClear = nullptr;
            platform::Log(L"member movement: the engine raised an exception clearing player 2's chocobo rows; rows are left "
                          L"to show this session");
            return;
        }
    }
}

RideTick TickRide(void* playerTwoController, void* member, void* state, const void* component, float deltaTime,
                  std::uint64_t withoutKey) noexcept
{
    const int type = MemberType(member);
    if (!generation::OnGameThread() || g_tickForCharacter == nullptr || type == kNoType ||
        ReadPointer(playerTwoController, facts::kControllerLeaderActions) == nullptr)
    {
        return RideTick::Refused;
    }
    const pad_sticks::StickScope    sticks(pad_sticks::ControllerIdOf(playerTwoController));
    const primary_controller::Scope primary;
    if (!sticks.Held() || !primary.Held())
    {
        g_counters.Refuse();
        return RideTick::Refused;
    }
    const pad_buttons::ButtonScope     buttons(withoutKey);
    const LeaderAnswer                 leader(type, component);
    const move_state::OwnTick          own;
    const scope_leaks::MemberTickScope leaks(state, playerTwoController, deltaTime);
    g_counters.Act();
    return GuardedTick(state, deltaTime) ? RideTick::Ticked : RideTick::Faulted;
}

HandlesTick TickRideHandles(const void* member, void* state, const void* chocobo, const void* component, float deltaTime,
                            HandlesFn engine, bool sample) noexcept
{
    HandlesTick tick;
    const int   type = MemberType(member);
    if (!generation::OnGameThread() || engine == nullptr || type == kNoType)
    {
        return tick;
    }
    if (sample)
    {
        tick.riddenOutside = RiddenTest(chocobo);
    }
    const primary_controller::Scope primary;
    if (!primary.Held())
    {
        return tick;  // counted by the ride (player_two/chocobo_ride), not as a member tick
    }
    const LeaderAnswer leader(type, component);
    if (sample)
    {
        tick.riddenInside = RiddenTest(chocobo);
    }
    tick.result = GuardedHandles(engine, state, deltaTime, tick.returned) ? RideTick::Ticked : RideTick::Faulted;
    return tick;
}

CoreAsPlayerTwo PlayerCoreAsPlayerTwo(const void* character, const void* member) noexcept
{
    CoreAsPlayerTwo result;
    const int       type = MemberType(member);
    if (!generation::OnGameThread() || g_isPlayerCore == nullptr || character == nullptr || type == kNoType)
    {
        return result;
    }
    const primary_controller::Scope primary;
    if (!primary.Held())
    {
        return result;
    }
    const unsigned     before = g_leaderAnswers.load(std::memory_order_relaxed);
    const LeaderAnswer leader(type);
    result.primary      = primary_controller::Current();
    std::uint8_t answer = 0;
    if (platform::CallGuarded(g_isPlayerCore, answer, character))
    {
        result.answer = answer;
    }
    // The answers counted here are not a tick's (game thread only, so nothing else counted meanwhile).
    const unsigned after = g_leaderAnswers.load(std::memory_order_relaxed);
    g_leaderAnswers.fetch_sub(after > before ? after - before : 0u, std::memory_order_relaxed);
    return result;
}

ThreadLeaderAnswer::ThreadLeaderAnswer(int type) noexcept : saved_(t_threadType)
{
    t_threadType = type;
}

ThreadLeaderAnswer::~ThreadLeaderAnswer()
{
    t_threadType = saved_;
}

bool LeaderTestReplaced() noexcept
{
    return g_leaderReplaced.load(std::memory_order_acquire);
}

int ExchangeThreadOnlyLeader(int type) noexcept
{
    const int previous = t_onlyType;
    t_onlyType         = type;
    return previous;
}

int ExchangeThreadLeaderAnswer(int type) noexcept
{
    const int previous = t_threadType;
    t_threadType       = type;
    return previous;
}

int ComponentPlayerType(const void* component) noexcept
{
    return component == nullptr ? kNoType : PlayerTypeOfComponent(component);
}

}  // namespace lss::player_two::member_movement
