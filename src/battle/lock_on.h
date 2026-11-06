#pragma once

// Each seat's targeting: the game's own targeting tick run again, for that seat
// (RE-p2-targeting-scoped-tick.md, pass S-TGT5; the fields in RE-p2-lock-on.md, S-TGT2).
//
// The game keeps one set of target fields in the battle system: the target cursor (+0x68), the
// displayed target (+0x70/+0x78, a TWeakPtr), the trace handle (+0x80), the lock byte (+0x8C), the
// switch latch (+0x8D) and the lock camera's rest timer (+0x90). Its targeting tick (soft target,
// lock toggle and lock camera, displayed target, stick switching, cursor) runs from two calls in the
// battle tick, each behind the game's own gates (in battle; time stopped only for the menu's target
// select; the suspend byte +0x96 inside).
//
// Each seat gets its own copy of those fields. A TargetScope puts a seat's into the battle system
// for as long as it lives and takes them back out at its end (the displayed target's weak reference
// moves with it), with player 1's put back; if the seat's lock changed inside, player 1's is put back
// through the game's own lock setter, which also sets the game-wide Global.LockOn parameter back to
// player 1's. It is held around the seat's member tick, the seat's menu input, the seat's camera
// update and the seat's HUD collection.
//
// Both targeting calls are redirected: the game's call runs for player 1 (on pad 1's own right stick
// while a seat plays; the merged stick is whichever pad is pushed furthest), then the same call runs
// again for each seat inside that seat's scopes: the seat's UI context (battle/focus_scope.h: the
// command controller slot answers the seat's command controller, and the focus path is the seat's, from
// which an open target list answers its selected row), the seat's pad's sticks and buttons, the seat as the primary
// controller (so the lock camera is the seat's camera manager's), the party leader and its type answering the seat's
// member, a TargetScope, and the bar manager's targeted key (EndUISystem[31] +0x140) as the seat's. Three
// writes of the tick have one copy for all players and are skipped in a seat's run: the two
// requests for player 1's target panel (their weak reference released as the request would) and the
// marker list's target owner name. A seat's run pauses while player 1 picks a target in the menu
// (the menu's target select is player 1's widgets). A seat's lock ends with the battle, as the
// game's teardown ends player 1's. Every state change is logged. Game thread only.
//
// Per seat: every seat has its own copy of the target fields and the targeted key
// (indexed by the seat; index 0 unused, player 1's are the game's own), its own TargetScope and its own lock's end.
// One TargetScope at a time, whichever seat's (the seats swap in turn). After the game's run, one run per seat in seat
// order (a third run is as safe as the second: RE-seats-step6-battle.md §1).

#include "platform/byte_scope.h"
#include "platform/dev_switch.h"
#include "players/seats.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::lock_on {

bool Install(const platform::GameImage& image) noexcept;

/// Player 1's targeting calls and the skipped writes back to the game's; no TargetScope holds after it.
void Remove() noexcept;

/// In battle, a seat's target fields in the battle system while it lives (a scope inside another, whichever
/// seat's, does nothing). The seat's are read back at its end and player 1's put back: five Exchange ranges of
/// platform/byte_scope.h, and the lock byte by hand (player 1's goes back through the game's setter
/// when the seat's changed).
class TargetScope
{
public:
    explicit TargetScope(int seat) noexcept;
    ~TargetScope();
    TargetScope(const TargetScope&)            = delete;
    TargetScope& operator=(const TargetScope&) = delete;

    bool Held() const noexcept { return system_ != 0; }

    /// Player 1's target cursor, saved while the seat's is in the battle system (for the log).
    std::uintptr_t OtherCursor() const noexcept { return cursor_; }

private:
    // The cursor, the displayed target, the trace handle, the rest timer and the switch latch.
    platform::ByteStore<5, 8 + 16 + 8 + 4 + 1> fields_;
    std::uintptr_t                             system_   = 0;
    std::uintptr_t                             cursor_   = 0;  // player 1's, for the log
    int                                        seat_     = 0;
    std::uint8_t                               lock_     = 0;  // player 1's
    std::uint8_t                               seatLock_ = 0;  // the seat's lock as the scope began
};

/// Inside `seat`'s member tick scopes, every tick: out of battle, the seat's lock, displayed
/// target and cursor end, as the game's battle teardown ends player 1's.
void AfterBattle(int seat) noexcept;

/// Diagnosis: the seats' targeting run on or off (on at start).
void SetEnabled(bool on) noexcept;
/// The seats' targeting run acts: installed, switched on, and not turned off for the session by an
/// exception.
bool Enabled() noexcept;

/// `seat`'s player is leaving: its displayed target, targeted key and cursor released, its lock cleared.
void SeatOut(int seat) noexcept;

/// Whether a TargetScope holds now, whichever seat's (then the game is in battle).
bool Held() noexcept;

/// `seat`'s displayed target object (outside a TargetScope), or 0.
std::uintptr_t DisplayedObject(int seat) noexcept;

/// `seat`'s displayed target as a TWeakPtr {object, reference controller} (outside a TargetScope).
void Displayed(int seat, std::uintptr_t& object, std::uintptr_t& controller) noexcept;

/// `seat`'s target cursor widget (outside a TargetScope), or null.
void* Cursor(int seat) noexcept;

/// For the log: whether `seat`'s command target list was open at its last targeting run, and how many times it has
/// opened (a HUD module logs its pass afresh when this changes).
bool          TargetListOpen(int seat) noexcept;
std::uint32_t TargetListOpenings(int seat) noexcept;

#if LSS_DEV
/// The `battle-mask` command and reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::battle::lock_on
