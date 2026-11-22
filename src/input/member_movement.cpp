#include "input/member_movement.h"

#include "battle/battle_camera.h"
#include "battle/battle_input.h"
#include "battle/command_menu.h"
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
#include "players/actor_fields.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "rides/chocobo_ride.h"
#include "rides/own_ride.h"
#include "rides/scour_scent.h"
#include "rides/wheelie_ride.h"
#include "scopes/leader_answer.h"
#include "scopes/primary_controller.h"
#include "scopes/scope_leaks.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"
#include "world/field_pickup.h"
#include "world/shooting_targets.h"

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

// Whether a seat's last move-state tick, its member's or its ride's, ran in its UI context.
enum class TickUiState : std::uint8_t
{
    Unknown,  // no tick yet
    With,
    Without,
};

// Game thread only, by seat (index 0 unused).
std::array<TickUiState, players::kMaxPlayers> g_tickUi{};

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

/// The ladder's arguments for a move-state tick of `seat`'s, whose player controller is `controller`: the seat's pad,
/// and its command controller for its UI context.
seat_ladder::Args TickArgs(int seat, void* controller) noexcept
{
    seat_ladder::Args args;
    args.seat       = seat;
    args.padId      = pad_sticks::ControllerIdOf(controller);
    args.controller = battle::command_menu::Controller(seat);
    return args;
}

/// The UI context part of a move-state tick's ladder, optional. A state's tick reads the command menu through the UI
/// system's controller slot: its key records drop Dodge, Attack and Accel while a menu is open, and its sprint,
/// field-action and ride rows wait for the menu (research/RE-p2-field-menu-dodge.md). In the seat's context the slot
/// names the seat's controller, so the seat's own menu holds the seat's character as player 1's holds player 1's. A seat
/// without a controller has no context to open.
unsigned TickUi(const seat_ladder::Args& args) noexcept
{
    return args.controller != nullptr ? seat_ladder::Focus : 0u;
}

/// Logs `seat`'s first move-state tick and each change between running in its UI context and without it (the slot then
/// names player 1's controller, whose menu holds the seat's actions instead of the seat's own).
void NoteTickUi(int seat, const seat_ladder::SeatScopes& scopes) noexcept
{
    const bool        without = !scopes.Holds<seat_ladder::FocusPart>();
    const TickUiState now     = without ? TickUiState::Without : TickUiState::With;
    TickUiState&      was     = g_tickUi[static_cast<std::size_t>(seat)];
    if (now == was)
    {
        return;
    }
    const bool first = was == TickUiState::Unknown;
    was              = now;
    if (without)
    {
        platform::Log(L"member movement: {}'s character ticks without their UI context ({}): player 1's command menu "
                      L"holds their actions meanwhile, not their own",
                      players::SeatName(seat), scopes.Noted() != nullptr ? scopes.Noted() : L"no command controller");
    }
    else
    {
        platform::Log(L"member movement: {}'s character ticks in their UI context{}: their own command menu holds "
                      L"their actions",
                      players::SeatName(seat), first ? L"" : L" again");
    }
}

/// After the engine raised an exception inside a tick in the seat's UI context: what the context holds may be
/// mid-change, so it is not kept (leaked, never put back), as at every other site that runs a game call in it.
void AbandonUi(seat_ladder::SeatScopes& scopes) noexcept
{
    if (auto* focus = scopes.Get<seat_ladder::FocusPart>())
    {
        focus->scope.Abandon();
    }
}

/// Ticks the seat's member's state inside every scope, or not at all.
void TickInScopes(int seat, void* controller, void* member, void* state, int type, float deltaTime) noexcept
{
    // The seat's sticks and its player as the primary controller, or no tick; its UI context, its own buttons (sprint,
    // dodge, field actions) and, in battle, the target system's fields as the seat's.
    const seat_ladder::Args args = TickArgs(seat, controller);
    const unsigned          ui   = TickUi(args);
    seat_ladder::SeatScopes scopes(
        ui | seat_ladder::Sticks | seat_ladder::Primary | seat_ladder::Buttons | seat_ladder::Targets,
        ui | seat_ladder::Buttons | seat_ladder::Targets, args);
    players::StageCounters& counters = g_counters[static_cast<std::size_t>(seat)];
    if (!scopes.Held())
    {
        counters.Refuse();
        return;
    }
    NoteTickUi(seat, scopes);
    const scopes::leader_answer::Scope       leader(type);
    const move_state::OwnTick        own;
    const scopes::scope_leaks::MemberTickScope leaks(state, controller, deltaTime);
    // The seat's own slot-0 target in the field-action manager: the throw's gate in this tick compares the seat's
    // box with it (world/field_pickup.h).
    const world::field_pickup::SlotTargetHold slotTarget(seat, controller);
    counters.Act();
    // Before the tick, inside every scope (the seat's camera manager is the primary one here):
    camera::camera_override::Tick(seat, member);   // the member's animation camera overrides reach the seat's camera
    battle::lock_on::AfterBattle(seat);            // out of battle, the seat's lock ends
    battle::battle_camera::Watch();                   // every camera manager's operators, logged on change (dev tools)
    battle::battle_input::Watch(seat, member, state);  // the seat's battle state, logged on change (dev tools)
    // The breakable boxes' leader-near byte answered for the member before its throw selection runs in the tick, for
    // player 1 again after (world/shooting_targets).
    world::shooting_targets::RefreshFor(member);
    const bool ticked = GuardedTick(state, deltaTime);
    world::shooting_targets::RefreshForPlayerOne();
    if (!ticked)
    {
        AbandonUi(scopes);
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
    void* state = players::actor_fields::MoveState(member);
    if (VTableOf(state) != g_moveStateVTable || players::actor_fields::MoveStateCharacter(state) != member ||
        players::actor_fields::Controller(member) != controller ||
        players::actor_fields::LeaderActions(controller) == nullptr)
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
    // The leader answer's slot last among the checks: its replacement is what makes the tick a seat's.
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
        platform::Log(L"member movement: the field-action handle clear differs; player 2's or 3's member will add its own "
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
    // tick is the frame's tick for the seat's member too (seen live: the member stood still while player 1 rode).
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
    return g_counters[players::SeatBuilt(seat) ? static_cast<std::size_t>(seat) : 1];  // never out of range
}

std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept
{
    // The same type, no component: the guard restores both, as every other answer scope does.
    const scopes::leader_answer::Scope leader(scopes::leader_answer::GameType());
    return test(character);
}

namespace {

/// Runs `call` inside the seat's ladder parts `which` (Primary among them) and the member's leader answer, or not at
/// all.
bool InSeatScopes(int seat, unsigned which, const void* member, void (*call)(void* context), void* context) noexcept
{
    const int type = MemberType(member);
    if (!players::generation::OnGameThread() || type == kNoType)
    {
        return false;
    }
    seat_ladder::Args args;
    args.seat = seat;
    const seat_ladder::SeatScopes scopes(which, args);
    if (!scopes.Held())
    {
        return false;
    }
    const scopes::leader_answer::Scope leader(type);
    call(context);
    return true;
}

}  // namespace

bool InPrimaryScope(int seat, const void* member, void (*call)(void* context), void* context) noexcept
{
    return InSeatScopes(seat, seat_ladder::Primary, member, call, context);
}

namespace {

struct NotifyCall
{
    void (*notify)(void* actor, void* context);
    void* actor;
    void* context;
};

void RunNotify(void* context) noexcept
{
    const auto& call = *static_cast<const NotifyCall*>(context);
    call.notify(call.actor, call.context);
}

}  // namespace

bool NotifyInPrimaryScope(int seat, void (*notify)(void* actor, void* context), void* actor, void* context) noexcept
{
    NotifyCall call{notify, actor, context};
    return InPrimaryScope(seat, players::generation::MemberOf(seat), &RunNotify, &call);
}

bool InPrimaryScopeWithButtons(int seat, const void* member, void (*call)(void* context), void* context) noexcept
{
    return InSeatScopes(seat, seat_ladder::Primary | seat_ladder::Buttons, member, call, context);
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
            platform::Log(L"member movement: the engine raised an exception clearing player 2's or 3's chocobo rows; "
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
        players::actor_fields::LeaderActions(controller) == nullptr)
    {
        return RideTick::Refused;
    }
    players::StageCounters& counters = g_counters[static_cast<std::size_t>(seat)];
    seat_ladder::Args args = TickArgs(seat, controller);
    args.withoutKey        = withoutKey;
    const unsigned ui      = TickUi(args);
    seat_ladder::SeatScopes scopes(ui | seat_ladder::Sticks | seat_ladder::Primary | seat_ladder::Buttons,
                                   ui | seat_ladder::Buttons, args);
    if (!scopes.Held())
    {
        counters.Refuse();
        return RideTick::Refused;
    }
    NoteTickUi(seat, scopes);
    const scopes::leader_answer::Scope         leader(type, component);
    const move_state::OwnTick          own;
    const scopes::scope_leaks::MemberTickScope leaks(state, controller, deltaTime);
    const rides::scour_scent::PressWatch       press(seat);  // a Scent or Scour request rising here is this seat's
    counters.Act();
    if (!GuardedTick(state, deltaTime))
    {
        AbandonUi(scopes);
        return RideTick::Faulted;
    }
    return RideTick::Ticked;
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
