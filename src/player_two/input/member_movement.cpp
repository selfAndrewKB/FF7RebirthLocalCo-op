#include "player_two/input/member_movement.h"

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
#include "player_two/battle/battle_camera.h"
#include "player_two/battle/battle_input.h"
#include "player_two/battle/lock_on.h"
#include "player_two/camera/camera_override.h"
#include "player_two/generation.h"
#include "player_two/input/climb_context.h"
#include "player_two/input/move_state.h"
#include "player_two/input/pad_buttons.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/object_fields.h"
#include "player_two/rides/chocobo_ride.h"
#include "player_two/rides/own_ride.h"
#include "player_two/rides/wheelie_ride.h"
#include "player_two/scopes/leader_answer.h"
#include "player_two/scopes/p2_scope.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/scopes/scope_leaks.h"
#include "player_two/session/party_member.h"

#include <cstddef>

namespace lss::player_two::member_movement {

namespace {

using TickForCharacterFn = void (*)(void* moveState, float deltaTime);
using LeaderTestFn       = std::uint8_t (*)(const void* component);
using PlayerTypeFn       = std::uint8_t (*)(const void* component);
using HandleClearFn      = void (*)(void* handle);

using leader_answer::kNoType;

// Set once at install.
TickForCharacterFn    g_tickForCharacter = nullptr;
HandleClearFn         g_handleClear      = nullptr;  // null: player 2's field-action rows are left to show
LeaderTestFn          g_isPlayerCore     = nullptr;  // IsUCPC_Core(character), for the ride logging only
LeaderTestFn          g_isRiddenVehicle  = nullptr;  // IsActiveRiddenVehicle(character), for the ride logging only
std::uintptr_t        g_moveStateVTable  = 0;

// Game thread only: set once the engine raised an exception inside the member's tick.
bool g_stopped = false;

StageCounters g_counters;

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
    // Pad 2's sticks and player 2 as the primary controller, or no tick; player 2's own buttons (sprint, dodge,
    // field actions) and, in battle, the target system's fields as player 2's.
    p2_scope::Args args;
    args.padId = pad_sticks::ControllerIdOf(playerTwoController);
    const p2_scope::P2Scope scopes(p2_scope::Sticks | p2_scope::Primary | p2_scope::Buttons | p2_scope::Targets,
                                   p2_scope::Buttons | p2_scope::Targets, args);
    if (!scopes.Held())
    {
        g_counters.Refuse();
        return;
    }
    const leader_answer::Scope       leader(type);
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
    const auto tick   = platform::FindFunction(image, facts::kMoveStateTickForCharacter);
    const auto vtable = image.Rebase(facts::kMoveStateVTable);
    // The leader answer's slot last among the checks: its replacement is what makes the tick player 2's.
    if (!tick || !vtable || !party_member::Installed() || !primary_controller::Installed() ||
        !pad_sticks::Installed() || !leader_answer::Install(image))
    {
        return false;
    }
    g_tickForCharacter = reinterpret_cast<TickForCharacterFn>(*tick);
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
    return true;
}

void Remove() noexcept
{
    leader_answer::Remove();
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
        own_ride::Release(L"the member's tick stopped (per-pad sticks switched off, or the tick stopped)");
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

std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept
{
    // The same type, no component: the guard restores both, as every other answer scope does.
    const leader_answer::Scope leader(leader_answer::GameType());
    return test(character);
}

bool InPrimaryScope(const void* member, void (*call)(void* context), void* context) noexcept
{
    const int type = MemberType(member);
    if (!generation::OnGameThread() || type == kNoType)
    {
        return false;
    }
    const primary_controller::Scope primary(players::kPlayerTwoSeat);
    if (!primary.Held())
    {
        return false;
    }
    const leader_answer::Scope leader(type);
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
    p2_scope::Args args;
    args.padId      = pad_sticks::ControllerIdOf(playerTwoController);
    args.withoutKey = withoutKey;
    const p2_scope::P2Scope scopes(p2_scope::Sticks | p2_scope::Primary | p2_scope::Buttons, p2_scope::Buttons, args);
    if (!scopes.Held())
    {
        g_counters.Refuse();
        return RideTick::Refused;
    }
    const leader_answer::Scope         leader(type, component);
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
    const primary_controller::Scope primary(players::kPlayerTwoSeat);
    if (!primary.Held())
    {
        return tick;  // counted by the ride (player_two/chocobo_ride), not as a member tick
    }
    const leader_answer::Scope leader(type, component);
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
    const primary_controller::Scope primary(players::kPlayerTwoSeat);
    if (!primary.Held())
    {
        return result;
    }
    const unsigned             before = leader_answer::Answers();
    const leader_answer::Scope leader(type);
    result.primary      = primary_controller::Current();
    std::uint8_t answer = 0;
    if (platform::CallGuarded(g_isPlayerCore, answer, character))
    {
        result.answer = answer;
    }
    // The answers counted here are not a tick's (game thread only, so nothing else counted meanwhile).
    const unsigned after = leader_answer::Answers();
    leader_answer::Uncount(after > before ? after - before : 0u);
    return result;
}

}  // namespace lss::player_two::member_movement
