#include "input/member_movement.h"

#include "battle/battle_camera.h"
#include "battle/battle_input.h"
#include "battle/lock_on.h"
#include "camera/camera_override.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "input/climb_context.h"
#include "input/move_state.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/every.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "rides/chocobo_ride.h"
#include "rides/own_ride.h"
#include "rides/wheelie_ride.h"
#include "scopes/leader_answer.h"
#include "scopes/primary_controller.h"
#include "scopes/scope_leaks.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#include <array>
#include <cstddef>

namespace lss::input::member_movement {

namespace seat_ladder = scopes::seat_ladder;

namespace {

using TickForCharacterFn = void (*)(void* moveState, float deltaTime);
using LeaderTestFn       = std::uint8_t (*)(const void* component);
using PlayerTypeFn       = std::uint8_t (*)(const void* component);
using HandleClearFn      = void (*)(void* handle);

using scopes::leader_answer::kNoType;

// Set once at install.
TickForCharacterFn    g_tickForCharacter = nullptr;
HandleClearFn         g_handleClear      = nullptr;  // null: the seats' field-action rows are left to show
LeaderTestFn          g_isPlayerCore     = nullptr;  // IsUCPC_Core(character), for the ride logging only
LeaderTestFn          g_isRiddenVehicle  = nullptr;  // IsActiveRiddenVehicle(character), for the ride logging only
std::uintptr_t        g_moveStateVTable  = 0;

// Game thread only, by seat (index 0, player 1's, unused): set once the engine raised an exception inside the seat's
// member's tick.
std::array<bool, players::kMaxPlayers> g_stopped{};

std::array<players::StageCounters, players::kMaxPlayers> g_counters{};  // by seat; index 0 unused

int MemberType(const void* member) noexcept
{
    const auto type = session::party_member::PlayerTypeOf(member);
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
    const bool firstFurther = seat == players::kFirstFurtherSeat;  // the dev watches below are player 2's (plan step 7)
    // The seat's sticks and its player as the primary controller, or no tick; its own buttons (sprint, dodge, field
    // actions) and, in battle, the target system's fields as the seat's.
    seat_ladder::Args args;
    args.seat  = seat;
    args.padId = pad_sticks::ControllerIdOf(controller);
    const seat_ladder::SeatScopes scopes(
        seat_ladder::Sticks | seat_ladder::Primary | seat_ladder::Buttons | seat_ladder::Targets,
        seat_ladder::Buttons | seat_ladder::Targets, args);
    players::StageCounters& counters = g_counters[static_cast<std::size_t>(seat)];
    if (!scopes.Held())
    {
        counters.Refuse();
        return;
    }
    const scopes::leader_answer::Scope       leader(type);
    const move_state::OwnTick        own;
    const scopes::scope_leaks::MemberTickScope leaks(state, controller, deltaTime);
    counters.Act();
    // Before the tick, inside every scope (the seat's camera manager is the primary one here):
    camera::camera_override::Tick(seat, member);   // the member's animation camera overrides reach the seat's camera
    battle::lock_on::AfterBattle(seat);            // out of battle, the seat's lock ends
    if (firstFurther)
    {
        battle::battle_camera::Watch();                // both camera managers' operators, logged on change (dev tools)
        battle::battle_input::Watch(member, state);    // player 2's battle state, logged on change (dev tools)
    }
    if (!GuardedTick(state, deltaTime))
    {
        g_stopped[static_cast<std::size_t>(seat)] = true;
        return;
    }
    ClearFieldActionRows(seat, state);
    rides::wheelie_ride::AfterMemberTick(controller, member);  // a Wheelie walk-in the tick answered yes to: boarded here
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
    // The leader answer's slot last among the checks: its replacement is what makes the tick a further player's.
    if (!tick || !vtable || !session::party_member::Installed() || !scopes::primary_controller::Installed() ||
        !pad_sticks::Installed() || !scopes::leader_answer::Install(image))
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
        platform::Log(L"member movement: the field-action handle clear differs; a further player's member will add its own "
                      L"(dim) summon rows to the prompt list");
    }
    return true;
}

void Remove() noexcept
{
    scopes::leader_answer::Remove();
}

void Tick(int seat, void* controller, void* member) noexcept
{
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::MemberTick);
    if (!players::generation::OnGameThread() || !players::SeatBuilt(seat))
    {
        return;
    }
    const auto     engine    = move_state::TakeEngineTicks(seat);
    const auto     index     = static_cast<std::size_t>(seat);
    players::StageCounters& counters  = g_counters[index];
    if (!pad_sticks::Enabled() || !pad_sticks::PartActs(pad_sticks::kPartMemberTick) || !players::generation::Active(seat) ||
        g_tickForCharacter == nullptr || g_stopped[index])
    {
        // A ride left un-ticked would hold the seat's controller on a chocobo nobody steers or ends: given back.
        rides::own_ride::Release(seat, L"the member's tick stopped (per-pad sticks switched off, or the tick stopped)");
        return;
    }
    if (rides::chocobo_ride::Tick(controller, member, engine))
    {
        return;  // the seat's player drives their chocobo: its move state was ticked instead of the member's
    }
    if (rides::wheelie_ride::Tick(controller, member, engine))
    {
        return;  // the seat's player rides a Wheelie: its move state was ticked instead of the member's
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

players::StageCounters& Counters(int seat) noexcept
{
    return g_counters[players::SeatBuilt(seat) ? static_cast<std::size_t>(seat) : players::kFirstFurtherSeat];
}

std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept
{
    // The same type, no component: the guard restores both, as every other answer scope does.
    const scopes::leader_answer::Scope leader(scopes::leader_answer::GameType());
    return test(character);
}

bool InPrimaryScope(int seat, const void* member, void (*call)(void* context), void* context) noexcept
{
    const int type = MemberType(member);
    if (!players::generation::OnGameThread() || type == kNoType)
    {
        return false;
    }
    const scopes::primary_controller::Scope primary(seat);
    if (!primary.Held())
    {
        return false;
    }
    const scopes::leader_answer::Scope leader(type);
    call(context);
    return true;
}

void ClearRideRows(void* chocoboState) noexcept
{
    if (g_handleClear == nullptr || chocoboState == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    for (const std::uint32_t offset : facts::kChocoboMoveStateRowHandles)
    {
        if (!GuardedClear(static_cast<std::byte*>(chocoboState) + offset))
        {
            g_handleClear = nullptr;
            platform::Log(L"member movement: the engine raised an exception clearing a further player's chocobo rows; "
                          L"rows are left to show this session");
            return;
        }
    }
}

RideTick TickRide(int seat, void* controller, void* member, void* state, const void* component, float deltaTime,
                  std::uint64_t withoutKey) noexcept
{
    const int type = MemberType(member);
    if (!players::SeatBuilt(seat) || !players::generation::OnGameThread() || g_tickForCharacter == nullptr || type == kNoType ||
        ReadPointer(controller, facts::kControllerLeaderActions) == nullptr)
    {
        return RideTick::Refused;
    }
    players::StageCounters& counters = g_counters[static_cast<std::size_t>(seat)];
    seat_ladder::Args args;
    args.seat       = seat;
    args.padId      = pad_sticks::ControllerIdOf(controller);
    args.withoutKey = withoutKey;
    const seat_ladder::SeatScopes scopes(seat_ladder::Sticks | seat_ladder::Primary | seat_ladder::Buttons,
                                         seat_ladder::Buttons, args);
    if (!scopes.Held())
    {
        counters.Refuse();
        return RideTick::Refused;
    }
    const scopes::leader_answer::Scope         leader(type, component);
    const move_state::OwnTick          own;
    const scopes::scope_leaks::MemberTickScope leaks(state, controller, deltaTime);
    counters.Act();
    return GuardedTick(state, deltaTime) ? RideTick::Ticked : RideTick::Faulted;
}

HandlesTick TickRideHandles(int seat, const void* member, void* state, const void* chocobo, const void* component,
                            float deltaTime, HandlesFn engine, bool sample) noexcept
{
    HandlesTick tick;
    const int   type = MemberType(member);
    if (!players::generation::OnGameThread() || engine == nullptr || type == kNoType)
    {
        return tick;
    }
    if (sample)
    {
        tick.riddenOutside = RiddenTest(chocobo);
    }
    const scopes::primary_controller::Scope primary(seat);
    if (!primary.Held())
    {
        return tick;  // counted by the ride (rides/chocobo_ride), not as a member tick
    }
    const scopes::leader_answer::Scope leader(type, component);
    if (sample)
    {
        tick.riddenInside = RiddenTest(chocobo);
    }
    tick.result = GuardedHandles(engine, state, deltaTime, tick.returned) ? RideTick::Ticked : RideTick::Faulted;
    return tick;
}

CoreAsSeat PlayerCoreAsSeat(int seat, const void* character, const void* member) noexcept
{
    CoreAsSeat result;
    const int       type = MemberType(member);
    if (!players::generation::OnGameThread() || g_isPlayerCore == nullptr || character == nullptr || type == kNoType)
    {
        return result;
    }
    const scopes::primary_controller::Scope primary(seat);
    if (!primary.Held())
    {
        return result;
    }
    const unsigned             before = scopes::leader_answer::Answers();
    const scopes::leader_answer::Scope leader(type);
    result.primary      = scopes::primary_controller::Current();
    std::uint8_t answer = 0;
    if (platform::CallGuarded(g_isPlayerCore, answer, character))
    {
        result.answer = answer;
    }
    // The answers counted here are not a tick's (game thread only, so nothing else counted meanwhile).
    const unsigned after = scopes::leader_answer::Answers();
    scopes::leader_answer::Uncount(after > before ? after - before : 0u);
    return result;
}

}  // namespace lss::input::member_movement
