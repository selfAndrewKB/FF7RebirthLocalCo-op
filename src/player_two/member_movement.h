#pragma once

// Moving player 2's party member with player 2's pad.
//
// The game's field tick runs one move state a frame, the party leader's, so the member player 2
// possesses never reads a stick. Once per frame, after the game's ticker has run, this ticks the
// member's move state with the game's own per-character tick, inside three scopes:
//   - player 2's pad as the merged stick (player_two/pad_sticks.h);
//   - player 2 as the primary player, so the tick takes its direction from player 2's camera
//     (player_two/primary_controller.h);
//   - a yes from the movement gate's party-leader test for the member's player type only
//     (a replaced vtable slot that answers on the game thread, inside this tick, and nowhere else).
// It runs only while per-pad sticks are on. Afterwards the member's field-action rows (the party's
// summons, which the tick asks for from whichever state it runs) are cleared with the game's own
// handle clear, as for any character the game did not tick; otherwise they would show as extra dim
// rows in the one shared prompt list.
//
// Game thread only.

#include "player_two/stage_counters.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::member_movement {

/// Finds the tick and replaces the leader-test slot. False (logged) if a check fails.
bool Install(const platform::GameImage& image) noexcept;

void Remove() noexcept;

/// Once per game tick while player 2 plays `member` with `playerTwoController`.
void Tick(void* playerTwoController, void* member) noexcept;

/// acted: the member was ticked with every scope held; refused: not ticked because a scope could
/// not be held or a precondition failed; passed: not ticked because the game's ticker did not run
/// or ticked the member itself. If the engine raises an exception inside the tick, the tick stops
/// for the rest of the session (logged).
StageCounters& Counters() noexcept;

/// Times the leader test answered yes for the member since the last call.
unsigned TakeLeaderAnswers() noexcept;

/// Logging (RE-p2-chocobo-ride.md §7.2 item 2): the game's IsUCPC_Core(`character`) as player 2's chocobo swap
/// would see it, inside the primary-controller scope and the member's leader answer, and the primary
/// controller seen there. Calls nothing else. -1 when a scope could not be held or the test is unavailable.
/// Game thread.
struct CoreAsPlayerTwo
{
    std::int32_t answer  = -1;
    const void*  primary = nullptr;
};
CoreAsPlayerTwo PlayerCoreAsPlayerTwo(const void* character, const void* member) noexcept;

/// The game's `test(character)` (IsUCPC_Core) with the leader test's yes for player 2's ridden chocobo's input
/// component held off, so the chocobo answers as player 1's ridden chocobo does (no). The member's type answer stays.
/// For the vault's camera check (player_two/vault_camera). Game thread.
std::uint8_t WithoutChocoboAnswer(std::uint8_t (*test)(const void* character), const void* character) noexcept;

/// Runs `call(context)` inside the primary-controller scope and the member's leader answer (player_two/chocobo_ride
/// runs the game's ride swap there). False, calling nothing, when the scope could not be held. Game thread.
bool InPrimaryScope(const void* member, void (*call)(void* context), void* context) noexcept;

/// Removes the rows of player 2's ridden chocobo's own prompt handles with the game's handle clear (as the chocobo
/// state's own deactivate does), at the end of player 2's ride. Game thread.
void ClearRideRows(void* chocoboState) noexcept;

/// Ticks player 2's ridden chocobo's move state with the game's per-character tick, inside every scope of the
/// member's tick (pad 2, primary, buttons, the mod's own tick, the scope-leak put-backs), the leader answer also
/// saying yes for `component` (the chocobo's input component), and player 2's buttons without `withoutKey` (an FName;
/// 0 for none). Game thread.
enum class RideTick
{
    Ticked,
    Refused,  // a scope could not be held
    Faulted,  // the engine raised an exception inside the tick
};
RideTick TickRide(void* playerTwoController, void* member, void* state, const void* component, float deltaTime,
                  std::uint64_t withoutKey) noexcept;

/// Runs `engine`, the game's slot 1 of player 2's ridden chocobo's move state (called by the chocobo's own actor tick:
/// its handles and the anim sync), inside the primary-controller scope and the leader answer for the member's type and
/// `component`: the context the game gives the chocobo player 1 rides (RE-p2-chocobo-run-refresh.md §4, lever 1).
/// Refused calls nothing (the caller runs `engine` as the game would). With `sample`, `ridden` gets the game's
/// ridden-vehicle test (IsActiveRiddenVehicle) for `chocobo` outside and inside the scope (-1: not evaluated).
/// Game thread.
using HandlesFn = std::uintptr_t (*)(void* state, float deltaTime);
struct HandlesTick
{
    RideTick       result        = RideTick::Refused;
    std::uintptr_t returned      = 0;  // the engine's rax, passed through
    std::int32_t   riddenOutside = -1;
    std::int32_t   riddenInside  = -1;
};
HandlesTick TickRideHandles(const void* member, void* state, const void* chocobo, const void* component, float deltaTime,
                            HandlesFn engine, bool sample) noexcept;

/// Answers yes to the leader test on this thread only, for components of player type `type` (-1: none), while it
/// lives; nesting keeps the outer answer. For the actions of player 2's ridden chocobo that tick on the anim worker
/// (player_two/rumble_list_two): their gate asks whether the chocobo's rider is the player, and the rider is the member.
/// Any thread.
class ThreadLeaderAnswer
{
public:
    explicit ThreadLeaderAnswer(int type) noexcept;
    ~ThreadLeaderAnswer();
    ThreadLeaderAnswer(const ThreadLeaderAnswer&)            = delete;
    ThreadLeaderAnswer& operator=(const ThreadLeaderAnswer&) = delete;

private:
    int saved_;
};

}  // namespace lss::player_two::member_movement
