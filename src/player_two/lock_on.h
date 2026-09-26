#pragma once

// Player 2's hard lock-on, right-stick target switching and lock-on camera (RE-p2-lock-on.md, pass
// S-TGT2).
//
// The game keeps one set of target fields in the battle system: the displayed target (+0x70/+0x78, a
// TWeakPtr), the lock byte (+0x8C), the switch latch (+0x8D) and the target cursor (+0x68). Its
// lock-camera tick reads the TargetLock press, toggles the lock (which also writes the game-wide
// parameter Global.LockOn), then keeps the LockOn camera module (layer 7) on or off on the primary
// camera manager. The camera module caches the displayed target when it activates and reads it again
// every frame.
//
// Player 2 gets their own copy of those fields. A TargetScope puts player 2's into the battle system
// for as long as it lives and takes them back out at its end (the displayed target's weak reference
// moves with it), with player 1's put back. It is held around player 2's member tick (so the game's
// own post-action refocus and stick-start code there use player 2's target and lock, and never touch
// player 1's cursor) and around player 2's camera update (so the LockOn module frames player 2's
// target). Inside the member tick, Tick then does what the game's tick does, with player 2's pad,
// member and camera manager: the TargetLock press flips player 2's lock (the game's toggle less the
// Global.LockOn write), the lock camera follows the game's rules through the primary-camera wrappers
// (player 2's camera manager there), the game's own switch runs on pad 2's right stick, and player
// 2's displayed target follows the member's own target. Player 2's lock ends with the battle, as
// the game's does. Every state change is logged.
//
// Player 1's side: the game's targeting tick reads the right stick through the merged stick value
// (whichever pad is pushed furthest), so its two calls run on pad 1's own stick while player 2
// plays; otherwise pad 2 would switch player 1's locked target and move player 1's lock camera.
// Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::lock_on {

bool Install(const platform::GameImage& image) noexcept;

/// Player 1's targeting calls back to the game's.
void Remove() noexcept;

/// In battle, player 2's target fields in the battle system while it lives (nested scopes do
/// nothing). Player 2's are read back at its end and player 1's put back.
class TargetScope
{
public:
    TargetScope() noexcept;
    ~TargetScope();
    TargetScope(const TargetScope&)            = delete;
    TargetScope& operator=(const TargetScope&) = delete;

    bool Held() const noexcept { return system_ != 0; }

private:
    std::uintptr_t system_ = 0;
    std::uintptr_t cursor_ = 0;
    std::uintptr_t object_ = 0;
    std::uintptr_t controller_ = 0;
    std::uint8_t   lock_ = 0;
    std::uint8_t   latch_ = 0;
};

/// Which parts act.
struct Parts
{
    bool lock      = true;  // the TargetLock press toggles player 2's lock
    bool switching = true;  // pad 2's right stick switches player 2's locked target
    bool camera    = true;  // the LockOn module on player 2's camera follows player 2's lock and target
    bool cursor    = true;  // player 2's own target cursor (player_two/target_cursor.h)
};

/// Inside player 2's member tick scopes and a TargetScope, with the party leader answering the
/// member, after the soft target: the press, the lock camera, switching, the displayed target.
void Tick(void* member, void* component, float deltaTime, Parts parts) noexcept;

/// Player 2 is leaving: their displayed target and cursor released, their lock cleared.
void PlayerTwoOut() noexcept;

/// Whether a TargetScope holds now (then the game is in battle).
bool Held() noexcept;

/// Player 2's displayed target object (outside a TargetScope), or 0.
std::uintptr_t DisplayedObject() noexcept;

/// Player 2's displayed target as a TWeakPtr {object, reference controller} (outside a TargetScope).
void Displayed(std::uintptr_t& object, std::uintptr_t& controller) noexcept;

}  // namespace lss::player_two::lock_on
