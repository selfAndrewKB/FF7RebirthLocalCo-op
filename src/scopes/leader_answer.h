#pragma once

// The party-leader test answered for player 2 (RE-p2-enemy-target-bias.md §10.3, RE-p2-chocobo-ride.md §7.0 item 2,
// RE-p2-chocobo-run-refresh.md §4). The game's one test of "is this component the player's character" (the IsUCPC
// slot, facts::kIsPartyLeader) compares a component's player type with the party leader's; player 2's member is never
// the leader, so the slot holds this module's test, which answers from five sources, in priority order:
//   1. one type counts as the player on this thread and every other as not (ExchangeThreadOnly: an enemy's target
//      choice, battle/equal_targeting, battle/target_coin);
//   2. the climb's type on the anim worker (input/climb_context);
//   3. a type on this thread (ThreadScope, ExchangeThread: player 2's chocobo's actions on the anim worker, a damage
//      call of player 2's);
//   4. the member tick's type, on the game thread only (Scope: input/member_movement's ticks and scopes);
//   5. one component, on the game thread only (Scope: player 2's ridden chocobo's input component, whose type is
//      every chocobo's).
// A no is the engine's own answer unless source 1 says otherwise. Decide is that rule, pure and tested on the host
// (tests/leader_answer_test.cpp); the module keeps the slot, the sources and the engine's own test.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::scopes::leader_answer {

inline constexpr int kNoType = -1;

enum class Verdict
{
    Yes,
    No,
    Engine,  // the game's own test decides
};

struct Answer
{
    Verdict verdict = Verdict::Engine;
    bool    counted = false;  // a yes of the member's (the status line counts them)
};

/// The rule. `onlyType`: source 1 (kNoType: none). `climbType` and `threadType`: sources 2 and 3 (the climb's first).
/// `gameType`: source 4; `extraPresent` and `extraMatches`: source 5 (a component is named; it is this one).
/// `typeOf()` reads the component's player type, called only when a source compares it.
template <class TypeOf>
constexpr Answer Decide(int onlyType, int climbType, int threadType, int gameType, bool extraPresent,
                        bool extraMatches, bool onGameThread, TypeOf&& typeOf) noexcept
{
    if (onlyType != kNoType)
    {
        return {typeOf() == onlyType ? Verdict::Yes : Verdict::No, false};
    }
    const int hereType = climbType != kNoType ? climbType : threadType;
    if (hereType != kNoType && typeOf() == hereType)
    {
        return {Verdict::Yes, true};
    }
    if ((gameType != kNoType || extraPresent) && onGameThread &&
        (extraMatches || (gameType != kNoType && typeOf() == gameType)))
    {
        return {Verdict::Yes, true};
    }
    return {};
}

/// Finds the engine's test and replaces the slot with this module's. False if a check fails: the member-movement
/// stage then stays uninstalled, which the session's stage line says.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the slot back (logged if it was changed after us).
void Remove() noexcept;

/// The slot holds this module's test (Install succeeded and it was not removed): the thread answers reach the game's
/// IsUCPC_Core only then.
bool Replaced() noexcept;

/// Answers yes for one player type (and one component, if given) while it lives: sources 4 and 5. Game thread.
/// Nesting keeps the outer answer.
class Scope
{
public:
    explicit Scope(int type, const void* component = nullptr) noexcept;
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int         savedType_;
    const void* savedComponent_;
};

/// The type source 4 answers for now (kNoType outside a Scope). Game thread.
int GameType() noexcept;

/// Answers yes to the leader test on this thread only, for components of player type `type` (kNoType: none), while it
/// lives: source 3. Nesting keeps the outer answer. Any thread.
class ThreadScope
{
public:
    explicit ThreadScope(int type) noexcept;
    ~ThreadScope();
    ThreadScope(const ThreadScope&)            = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;

private:
    int saved_;
};

/// Source 1 as a function: one type counts as the player on this thread, every other as not, over every other
/// source; kNoType ends it. Returns the previous value, which the caller puts back (nesting keeps the outer answer). A
/// function rather than a scope object so the caller can put it back in a __finally, which also runs when a
/// structured exception unwinds (the mod builds with /EHsc, where destructors would not). Any thread.
int ExchangeThreadOnly(int type) noexcept;

/// ThreadScope as a function (source 3), for a __finally as above. Any thread.
int ExchangeThread(int type) noexcept;

/// The player type `component` (a battle-control component) reports through its own type slot, or kNoType.
int ComponentPlayerType(const void* component) noexcept;

/// The counted yeses since the last Take (the session's status line), the count now, and a correction for answers
/// counted by a probe that was not a tick's (input/member_movement's boarding gate).
unsigned TakeAnswers() noexcept;
unsigned Answers() noexcept;
void     Uncount(unsigned answers) noexcept;

}  // namespace lss::scopes::leader_answer
