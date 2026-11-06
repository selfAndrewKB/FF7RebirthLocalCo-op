#include "input/move_state.h"

#include "facts/move_state.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "input/move_census.h"
#include "input/pad_sticks.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "rides/chocobo_ride.h"

#include <array>
#include <cstddef>

namespace lss::input::move_state {

namespace {

/// Slot 11 of both move-state vtables. Callers ignore the result; rax is passed through whole.
using TickFn = std::uintptr_t (*)(void* state, float deltaTime);

// Set once at install.
platform::VTableEntry g_tickSlot;
platform::VTableEntry g_tickInputSlot;
platform::VTableEntry g_updateInputSlot;
platform::VTableEntry g_playerUpdateInputSlot;
platform::VTableEntry g_leaderActionTickSlot;
platform::VTableEntry g_chocoboTickSlot;
platform::VTableEntry g_chocoboUpdateInputSlot;
TickFn                g_engineChocoboTick        = nullptr;  // the chocobo move state's slot 11
TickFn                g_engineChocoboUpdateInput = nullptr;  // ... and slot 16
TickFn                g_engineChocoboHandles     = nullptr;  // ... and slot 1: void(state, dt), rax passed through
platform::VTableEntry g_chocoboHandlesSlot;
platform::VTableEntry g_scooterUpdateInputSlot;
TickFn                g_engineScooterUpdateInput = nullptr;  // the scooter move state's slot 16
TickFn                g_engineTick      = nullptr;
TickFn                g_engineTickInput = nullptr;
TickFn                g_engineUpdateInput       = nullptr;  // slot 16, same shape as slot 11
TickFn                g_enginePlayerUpdateInput = nullptr;
TickFn                g_engineLeaderActionTick  = nullptr;  // (component, dt)

// Game thread only.
bool                                          g_ownTick = false;
std::array<EngineTicks, players::kMaxPlayers> g_engineTicks{};  // by seat; index 0 (player 1) unused

/// Each built seat's record, for the ticks every seat sees alike.
template <typename Fn>
void EachSeatRecord(Fn&& fn) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        fn(seat, g_engineTicks[static_cast<std::size_t>(seat)]);
    }
}

/// Runs one move-state tick. On the game thread it is counted, and the game's own ticks are
/// recorded for every seat; with per-pad sticks on, player 1's character reads only player 1's pad.
/// (A seat's member is ticked by the mod with its scopes already open: input/member_movement.h.)
std::uintptr_t RunForOwner(TickFn engine, void* state, float deltaTime, bool viaTickInput) noexcept
{
    if (!players::generation::OnGameThread())
    {
        return engine(state, deltaTime);
    }
    void* character = ReadPointer(state, facts::kMoveStateCharacter);
    if (!g_ownTick)
    {
        EachSeatRecord([&](int seat, EngineTicks& ticks) {
            ++ticks.count;
            ticks.deltaTime = deltaTime;
            ticks.memberTicked |= character != nullptr && character == players::generation::MemberOf(seat);
        });
    }
    move_census::Count(state, character, viaTickInput, g_ownTick);  // the census (dev tools)

    const void* owner = ReadPointer(character, facts::kCharacterEffectiveController);
    if (!pad_sticks::Enabled() || !pad_sticks::PartActs(pad_sticks::kPartPlayerOneMove) || !players::generation::AnyActive() ||
        owner == nullptr ||
        owner != players::generation::ControllerOf(0))
    {
        return engine(state, deltaTime);
    }
    const pad_sticks::StickScope sticks(pad_sticks::ControllerIdOf(owner));
    sticks.Held() ? pad_sticks::MoveCounters().Act() : pad_sticks::MoveCounters().Refuse();
    return engine(state, deltaTime);
}

/// Runs `engine` with player 1's pad as the only stick when owner is player 1's controller and
/// per-pad sticks act on player 1's detection. Everything after slot 11 in player 1's tick reads the
/// stick too: the input update's terrain detection turns its probes toward it, and the leader-action
/// tick keeps its history (RE-p2-terrain-action-conflict.md §7). Unscoped, they read the merged stick,
/// which is a seat's pad's whenever only that seat's player moves. A scope already open (a seat's
/// member, or slot 11 inside slot 16) passes through.
std::uintptr_t RunOnPlayerOnesPad(TickFn engine, void* object, float deltaTime, const void* owner) noexcept
{
    if (!players::generation::OnGameThread() || !pad_sticks::Enabled() || !pad_sticks::PartActs(pad_sticks::kPartPlayerOneDetect) ||
        !players::generation::AnyActive() || owner == nullptr || owner != players::generation::ControllerOf(0))
    {
        return engine(object, deltaTime);
    }
    const pad_sticks::StickScope sticks(pad_sticks::ControllerIdOf(owner));
    return engine(object, deltaTime);
}

std::uintptr_t UpdateInputFor(TickFn engine, void* state, float deltaTime) noexcept
{
    const void* character = players::generation::OnGameThread() ? ReadPointer(state, facts::kMoveStateCharacter) : nullptr;
    return RunOnPlayerOnesPad(engine, state, deltaTime, ReadPointer(character, facts::kCharacterEffectiveController));
}

std::uintptr_t UpdateInput(void* state, float deltaTime)
{
    return UpdateInputFor(g_engineUpdateInput, state, deltaTime);
}

std::uintptr_t PlayerUpdateInput(void* state, float deltaTime)
{
    return UpdateInputFor(g_enginePlayerUpdateInput, state, deltaTime);
}

/// Player 1's ridden chocobo or scooter: the field tick runs its move state for player 1 (PC1 possesses it,
/// RE-p2-chocobo-ride.md §1.2, RE-p2-wheelie-ride.md §14.5); unscoped it read the merged stick, so any pad steered it
/// (live 2026-10-02, the chocobo). Not counted as an engine tick; recorded as the ride tick (for every seat), which a
/// seat's member tick and own ride run on. A follower's chocobo (its own AI controller) and a seat's own ride pass
/// through.
std::uintptr_t ChocoboFor(TickFn engine, void* state, float deltaTime, bool outer) noexcept
{
    const void* chocobo = players::generation::OnGameThread() ? ReadPointer(state, facts::kMoveStateCharacter) : nullptr;
    const void* owner   = ReadPointer(chocobo, facts::kCharacterEffectiveController);
    if (owner == nullptr)
    {
        owner = ReadPointer(chocobo, facts::kPawnController);
    }
    // Recorded from slot 16 only (the field tick's call, with its delta time; slot 11 runs inside it).
    if (outer && !g_ownTick && owner != nullptr && owner == players::generation::ControllerOf(0) && deltaTime > 0.0f &&
        deltaTime < 1.0f)
    {
        EachSeatRecord([&](int, EngineTicks& ticks) {
            ticks.rideTicked    = true;  // the field ticker ran player 1's ride body this frame
            ticks.rideDeltaTime = deltaTime;
        });
    }
    return RunOnPlayerOnesPad(engine, state, deltaTime, owner);
}

std::uintptr_t ChocoboTick(void* state, float deltaTime)
{
    return ChocoboFor(g_engineChocoboTick, state, deltaTime, false);
}

std::uintptr_t ChocoboUpdateInput(void* state, float deltaTime)
{
    return ChocoboFor(g_engineChocoboUpdateInput, state, deltaTime, true);
}

std::uintptr_t ScooterUpdateInput(void* state, float deltaTime)
{
    return ChocoboFor(g_engineScooterUpdateInput, state, deltaTime, true);
}

/// Every chocobo move state's slot 1, run by the chocobo's own actor tick in game context (its handles and the anim
/// sync). A seat's ridden chocobo's runs inside the seat's scopes (rides/chocobo_ride.h, TickHandles); the rest pass
/// through.
std::uintptr_t ChocoboHandles(void* state, float deltaTime)
{
    std::uintptr_t returned = 0;
    return rides::chocobo_ride::TickHandles(state, deltaTime, g_engineChocoboHandles, returned)
               ? returned
               : g_engineChocoboHandles(state, deltaTime);
}

std::uintptr_t LeaderActionTick(void* component, float deltaTime)
{
    const void* owner = players::generation::OnGameThread() ? ReadPointer(component, facts::kLeaderActionOwner) : nullptr;
    return RunOnPlayerOnesPad(g_engineLeaderActionTick, component, deltaTime, owner);
}

std::uintptr_t Tick(void* state, float deltaTime)
{
    return RunForOwner(g_engineTick, state, deltaTime, false);
}

std::uintptr_t TickInput(void* state, float deltaTime)
{
    return RunForOwner(g_engineTickInput, state, deltaTime, true);
}

/// Slot 16 of both move-state vtables and the leader-action tick (each independently; logged).
void InstallDetectionScopes(const platform::GameImage& image) noexcept
{
    struct Wrap
    {
        const platform::SlotFact& fact;
        TickFn&                   engine;
        std::uintptr_t            replacement;
        platform::VTableEntry&    slot;
    };
    const Wrap wraps[] = {
        {facts::kMoveStateUpdateInput, g_engineUpdateInput, reinterpret_cast<std::uintptr_t>(&UpdateInput), g_updateInputSlot},
        {facts::kPlayerMoveStateUpdateInput, g_enginePlayerUpdateInput, reinterpret_cast<std::uintptr_t>(&PlayerUpdateInput),
         g_playerUpdateInputSlot},
        {facts::kLeaderActionTick, g_engineLeaderActionTick, reinterpret_cast<std::uintptr_t>(&LeaderActionTick),
         g_leaderActionTickSlot},
        {facts::kChocoboMoveStateTick, g_engineChocoboTick, reinterpret_cast<std::uintptr_t>(&ChocoboTick), g_chocoboTickSlot},
        {facts::kChocoboMoveStateUpdateInput, g_engineChocoboUpdateInput,
         reinterpret_cast<std::uintptr_t>(&ChocoboUpdateInput), g_chocoboUpdateInputSlot},
        {facts::kScooterMoveStateUpdateInput, g_engineScooterUpdateInput,
         reinterpret_cast<std::uintptr_t>(&ScooterUpdateInput), g_scooterUpdateInputSlot},
    };
    int installed = 0;
    for (const Wrap& wrap : wraps)
    {
        const auto live = platform::FindFunction(image, wrap.fact.target);
        wrap.engine     = live ? reinterpret_cast<TickFn>(*live) : nullptr;
        installed += live && platform::ReplaceSlot(image, wrap.fact, wrap.replacement, wrap.slot) ? 1 : 0;
    }
    platform::Log(L"per-pad sticks: player 1's input update, leader-action tick and ridden chocobo and scooter on pad 1: {} "
                  L"of {} slots",
                  installed, std::size(wraps));
}

/// The chocobo move state's slot 1, for a seat's own ride (RE-p2-chocobo-run-refresh.md §4, lever 1; logged).
void InstallChocoboHandles(const platform::GameImage& image) noexcept
{
    const auto live          = platform::FindFunction(image, facts::kChocoboMoveStateHandles.target);
    g_engineChocoboHandles   = live ? reinterpret_cast<TickFn>(*live) : nullptr;
    const bool replaced      = live && platform::ReplaceSlot(image, facts::kChocoboMoveStateHandles,
                                                             reinterpret_cast<std::uintptr_t>(&ChocoboHandles),
                                                             g_chocoboHandlesSlot);
    if (replaced)
    {
        platform::Log(L"chocobo ride: a further player's chocobo's own slot 1 runs in that player's scope (the game's anim sync leaves "
                      L"its gait to that player's input)");
    }
    else
    {
        platform::Log(L"chocobo ride: the chocobo move state's slot 1 differs; a further player's chocobo's gait follows its "
                      L"animation (no sprint, run-on after a sprint attempt)");
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick      = platform::FindFunction(image, facts::kMoveStateTick.target);
    const auto tickInput = platform::FindFunction(image, facts::kMoveStateTickInput.target);
    g_engineTick         = tick ? reinterpret_cast<TickFn>(*tick) : nullptr;
    g_engineTickInput    = tickInput ? reinterpret_cast<TickFn>(*tickInput) : nullptr;
    const bool first  = tick && platform::ReplaceSlot(image, facts::kMoveStateTick,
                                                      reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot);
    const bool second = tickInput && platform::ReplaceSlot(image, facts::kMoveStateTickInput,
                                                           reinterpret_cast<std::uintptr_t>(&TickInput),
                                                           g_tickInputSlot);
    InstallDetectionScopes(image);
    InstallChocoboHandles(image);
    return first || second;
}

void Remove() noexcept
{
    for (auto* slot : {&g_tickSlot, &g_tickInputSlot, &g_updateInputSlot, &g_playerUpdateInputSlot, &g_leaderActionTickSlot,
                       &g_chocoboTickSlot, &g_chocoboUpdateInputSlot, &g_chocoboHandlesSlot, &g_scooterUpdateInputSlot})
    {
        if (!slot->Restore())
        {
            platform::Log(L"could not restore a move-state slot; the replacement stays in place");
        }
    }
}

EngineTicks TakeEngineTicks(int seat) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return {};
    }
    EngineTicks&      record = g_engineTicks[static_cast<std::size_t>(seat)];
    const EngineTicks taken  = record;
    record                   = {};
    return taken;
}

OwnTick::OwnTick() noexcept
{
    g_ownTick = true;
}

OwnTick::~OwnTick()
{
    g_ownTick = false;
}

}  // namespace lss::input::move_state
