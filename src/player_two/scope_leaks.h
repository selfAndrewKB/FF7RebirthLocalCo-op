#pragma once

// Putting back what the mod's two "as player 2" calls change outside player 2's own objects
// (RE-p2-scope-leak-audit.md, leaks 1, 2 and 5). The game runs this code for one player; after each
// call, the shared state is left as the game leaves it with one player.
//
//   Leak 1, the leader-action clock: the per-controller component's tick (EndPlayerLeader-
//     ActionParamComponent vt+0x428) advances three globals once per frame: a frame count, seconds
//     and ms, which about 80 input-history readers compare press stamps against. The game ticks
//     only player 1's component; the mod's tick of player 2's member ticks player 2's too, so the
//     clock would advance twice. MemberTickScope sets the clock back one step before that tick, so
//     the tick advances it onto the values player 1's tick produced this frame (player 2's press
//     stamps then carry the same frame), and writes the saved values back after (three Exchange
//     ranges of platform/byte_scope.h: what the tick left is read back to tell whether it advanced).
//   Leak 2, the leader transform: player 2's controller input (the component's vt+0x430) writes
//     player 2's member's transform and motion into the one global block that follower AI reads as
//     the leader's (its formation anchor). InputScope saves the block around player 2's input and
//     puts it back (a Keep range of platform/byte_scope.h).
//   Leak 5, the field summons: the move state's summon body (chocobo whistle, buggy call/return,
//     cactus statue, their row upgrades) reads the shared buttons, so player 1's press could call
//     or return a vehicle a second time through player 2's member. The game never runs it for a
//     character it does not tick; its one call is redirected, and skipped for player 2's member's
//     state inside the mod's tick.
//
// All Square Enix End code. Game thread only; addresses are this build's, each checked at install.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::scope_leaks {

inline constexpr unsigned kFixClock           = 1;
inline constexpr unsigned kFixLeaderTransform = 2;
inline constexpr unsigned kFixSummonBody      = 4;
inline constexpr unsigned kAllFixes           = kFixClock | kFixLeaderTransform | kFixSummonBody;

/// Checks each fix's facts and installs the summon redirect. Each fix that holds works on its own;
/// false (logged) if none does.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Diagnosis: which fixes act (bits above). All at start. Logged.
void SetMask(unsigned mask) noexcept;

/// Around player 2's controller input. Acts only for player 2's controller, on the game thread,
/// while player 2 is in.
class InputScope
{
public:
    explicit InputScope(bool playerTwo) noexcept;
    ~InputScope();
    InputScope(const InputScope&)            = delete;
    InputScope& operator=(const InputScope&) = delete;

private:
    bool held_ = false;
};

/// Around the mod's move-state tick of player 2's member (player_two/member_movement.h).
class MemberTickScope
{
public:
    MemberTickScope(void* state, const void* playerTwoController, float deltaTime) noexcept;
    ~MemberTickScope();
    MemberTickScope(const MemberTickScope&)            = delete;
    MemberTickScope& operator=(const MemberTickScope&) = delete;

private:
    bool clockHeld_ = false;
};

/// Writes one line of counts since the last call (the player 2 status line).
void LogStatus() noexcept;

}  // namespace lss::player_two::scope_leaks
