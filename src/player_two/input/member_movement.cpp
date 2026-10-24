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

#include <array>
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

// Game thread only, by seat (index 0, player 1's, unused): set once the engine raised an exception inside the seat's
// member's tick.
std::array<bool, players::kMaxPlayers> g_stopped{};

std::array<StageCounters, players::kMaxPlayers> g_counters{};  // by seat; index 0 unused

// Inside a seat's member tick, the rides (chocobo_ride, wheelie_ride, own_ride), the battle target fields (lock_on's
// Targets part and AfterBattle) and the dev watches are player 2's: Tick calls them for player 2's seat only.
static_assert(players::kSeatsBuilt == 1, "the rides, lock_on and the target fields are player 2's: before seat 2 is "
                                         "built their groups make them per seat and Tick calls them for every seat");

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
void ClearFieldActionRows(int seat, void* state) noexcept
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
            platform::Log(L"member movement: the engine raised an exception clearing {}'s field-action rows; "
                          L"they are left to show this session",
                          players::SeatName(seat));
            return;
        }
    }
}

/// Ticks the seat's member's state inside every scope, or not at all.
void TickInScopes(int seat, void* controller, void* member, void* state, int type, float deltaTime) noexcept
{
    const bool playerTwo = seat == players::kPlayerTwoSeat;
    // The seat's sticks and its player as the primary controller, or no tick; its own buttons (sprint, dodge, field
    // actions) and, in battle, the target system's fields as player 2's (lock_on: player 2's only).
    p2_scope::Args args;
    args.seat  = seat;
    args.padId = pad_sticks::ControllerIdOf(controller);
    const unsigned targets = playerTwo ? unsigned{p2_scope::Targets} : 0u;
    const p2_scope::P2Scope scopes(p2_scope::Sticks | p2_scope::Primary | p2_scope::Buttons | targets,
                                   p2_scope::Buttons | targets, args);
    StageCounters& counters = g_counters[static_cast<std::size_t>(seat)];
    if (!scopes.Held())
    {
        counters.Refuse();
        return;
    }
    const leader_answer::Scope       leader(type);
    const move_state::OwnTick        own;
    const scope_leaks::MemberTickScope leaks(state, controller, deltaTime);
    counters.Act();
    // Before the tick, inside every scope (the seat's camera manager is the primary one here):
    camera_override::Tick(member);         // the member's animation camera overrides reach the seat's camera
    if (playerTwo)
    {
        lock_on::AfterBattle();                // out of battle, player 2's lock ends
        battle_camera::Watch();                // both camera managers' operators, logged on change (dev tools)
        battle_input::Watch(member, state);    // player 2's battle state, logged on change (dev tools)
    }
    if (!GuardedTick(state, deltaTime))
    {
        g_stopped[static_cast<std::size_t>(seat)] = true;
        return;
    }
    ClearFieldActionRows(seat, state);
    if (playerTwo)
    {
        wheelie_ride::AfterMemberTick(controller, member);  // a Wheelie walk-in the tick answered yes to: boarded here
    }
}

/// The member's move state, if the member is the seat's possessed character (by `controller`) and the
/// tick's unchecked reads hold (the controller's leader-action component).
void* MoveStateToTick(void* controller, void* member) noexcept
{
    void* state = ReadPointer(member, facts::kCharacterMoveState);
    if (VTableOf(state) != g_moveStateVTable || ReadPointer(state, facts::kMoveStateCharacter) != member ||
        ReadPointer(member, facts::kPawnController) != controller ||
        ReadPointer(controller, facts::kControllerLeaderActions) == nullptr)
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

void Tick(int seat, void* controller, void* member) noexcept
{
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::MemberTick);
    if (!generation::OnGameThread() || !players::SeatBuilt(seat))
    {
        return;
    }
    const auto     engine    = move_state::TakeEngineTicks(seat);
    const auto     index     = static_cast<std::size_t>(seat);
    const bool     playerTwo = seat == players::kPlayerTwoSeat;
    StageCounters& counters  = g_counters[index];
    if (!pad_sticks::Enabled() || !pad_sticks::PartActs(pad_sticks::kPartMemberTick) || !generation::Active(seat) ||
        g_tickForCharacter == nullptr || g_stopped[index])
    {
        if (playerTwo)
        {
            // A ride left un-ticked would hold player 2's controller on a chocobo nobody steers or ends: given back.
            own_ride::Release(L"the member's tick stopped (per-pad sticks switched off, or the tick stopped)");
        }
        return;
    }
    if (playerTwo && chocobo_ride::Tick(controller, member, engine))
    {
        return;  // player 2 drives their chocobo: its move state was ticked instead of the member's
    }
    if (playerTwo && wheelie_ride::Tick(controller, member, engine))
    {
        return;  // player 2 rides a Wheelie: its move state was ticked instead of the member's
    }
    // While player 1 rides a scooter, the field ticker runs only its ride body (no on-foot state is counted): that ride
    // tick is the frame's tick for the seat's member too (live 2026-10-06: she stood still while player 1 rode).
    if ((engine.count == 0 && !engine.rideTicked) || engine.memberTicked)
    {
        counters.Pass();  // the game's field ticker is idle (a menu, an event), or ran the member itself
        return;
    }
    void*     state = MoveStateToTick(controller, member);
    const int type  = MemberType(member);
    if (state == nullptr || type == kNoType)
    {
        counters.Refuse();
        return;
    }
    TickInScopes(seat, controller, member, state, type, engine.count > 0 ? engine.deltaTime : engine.rideDeltaTime);
    if (g_stopped[index])
    {
        platform::Log(L"member movement STOPPED: the engine raised an exception inside the member's move-state tick; "
                      L"{}'s member is no longer ticked this session",
                      players::SeatName(seat));
    }
}

StageCounters& Counters(int seat) noexcept
{
    return g_counters[players::SeatBuilt(seat) ? static_cast<std::size_t>(seat) : players::kPlayerTwoSeat];
}

std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept
{
    // The same type, no component: the guard restores both, as every other answer scope does.
    const leader_answer::Scope leader(leader_answer::GameType());
    return test(character);
}

bool InPrimaryScope(int seat, const void* member, void (*call)(void* context), void* context) noexcept
{
    const int type = MemberType(member);
    if (!generation::OnGameThread() || type == kNoType)
    {
        return false;
    }
    const primary_controller::Scope primary(seat);
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
                          L"to show this session");  // ClearRideRows is player 2's (the rides' call)
            return;
        }
    }
}

RideTick TickRide(int seat, void* controller, void* member, void* state, const void* component, float deltaTime,
                  std::uint64_t withoutKey) noexcept
{
    const int type = MemberType(member);
    if (!players::SeatBuilt(seat) || !generation::OnGameThread() || g_tickForCharacter == nullptr || type == kNoType ||
        ReadPointer(controller, facts::kControllerLeaderActions) == nullptr)
    {
        return RideTick::Refused;
    }
    StageCounters& counters = g_counters[static_cast<std::size_t>(seat)];
    p2_scope::Args args;
    args.seat       = seat;
    args.padId      = pad_sticks::ControllerIdOf(controller);
    args.withoutKey = withoutKey;
    const p2_scope::P2Scope scopes(p2_scope::Sticks | p2_scope::Primary | p2_scope::Buttons, p2_scope::Buttons, args);
    if (!scopes.Held())
    {
        counters.Refuse();
        return RideTick::Refused;
    }
    const leader_answer::Scope         leader(type, component);
    const move_state::OwnTick          own;
    const scope_leaks::MemberTickScope leaks(state, controller, deltaTime);
    counters.Act();
    return GuardedTick(state, deltaTime) ? RideTick::Ticked : RideTick::Faulted;
}

HandlesTick TickRideHandles(int seat, const void* member, void* state, const void* chocobo, const void* component,
                            float deltaTime, HandlesFn engine, bool sample) noexcept
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
    const primary_controller::Scope primary(seat);
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

CoreAsPlayerTwo PlayerCoreAsSeat(int seat, const void* character, const void* member) noexcept
{
    CoreAsPlayerTwo result;
    const int       type = MemberType(member);
    if (!generation::OnGameThread() || g_isPlayerCore == nullptr || character == nullptr || type == kNoType)
    {
        return result;
    }
    const primary_controller::Scope primary(seat);
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
