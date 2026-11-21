#pragma once

// Each seat's command controller ticked at the game's own spot, and that seat's battle actions driven
// through it (research/RE-p2-command-menu-tick.md, RE-p2-command-menu.md).
//
// The game ticks its one command controller once per frame, from the game mode's phase-0 global tick
// (one call). That call is redirected: the game's controller ticks as before, then, in battle and
// out of it, each set-up seat's controller (battle/command_menu.h) ticks inside that seat's scopes: its
// focus context and the UI system's controller slot, the primary controller, the party leader
// (weak pointer and type) answering the seat's member, the seat's target fields, and the seat's pad's sticks
// and buttons. The controller's own sub-ticks then call the game's per-character attack, guard and
// dodge functions on the seat's member, so the seat's Attack, Unique, Guard and Dodge are written
// into its request array from the seat's pad (levels, as the game's input handler and up route leave them).
// This is the only path for a seat's actions; if it cannot run, the log says why.
//
// What a second tick would change for player 1 is handled:
//   - the world slow (slot 0, written by every tick): either menu slows the world (policy A, the
//     smaller of the two values is written); with the slow-down switched off (the menu-slow setting, off by
//     default) every write asks for full speed, the game's own value with no menu open, and the menu's grey and
//     muffle are skipped (battle/menu_tint);
//   - the per-type command snapshot: a seat's pass does not write it (its one reader cancels a
//     pending command on the game's controller); the key guide is the seat's own inside the scope;
//   - the leader-switch requests: the seat's own, as its pad's d-pad sets them; the tick's switch step then runs the
//     game's cycle for the seat, every leader read answering the seat (session/party_switch.h keeps it off player
//     1's and the other players' characters, and the session plays the member it moves to);
//   - the battle HUD mode: the game's own call on its controller (its setter's entry redirected)
//     is made again on each seat's controller at once, inside that seat's focus and leader scopes; a seat
//     that could not take it (its player out) is caught up at its next pass;
//   - battle input, party-list input and the menu's focus layer, which the game sets with no call to
//     repeat, are copied from the game's controller at each seat's pass through the game's own setters on
//     the seat's controller, never the global ones;
//   - the command rules are turned around for the pass: a seat's player may not command player 1's leader;
//   - outside battle the controller takes its menu member from battle member 0, player 1's character, not from
//     the leader type the seat's scope answers: in the command menu's own calls on a seat's controller (its passes, its
//     presses, the repeated setters, the lifecycle's Setup) that one read answers "none" inside the seat's
//     leader-type scope, so the game's own fallback gives the seat's member (research/RE-p2-field-command-menu.md),
//     and the seat's field menu lists its own member's commands, items and magic; every other module's leader-type
//     scope keeps the game's answer;
//   - the game's "menu allowed" call (minigames, summons, events forbid the menu) is made again on each seat's
//     controller as the HUD mode's is; one the game stores directly is caught up at the seat's next pass.
// A seat's pad's presses reach that seat's menu in battle and out of it; a release follows its press (the capture
// map's rule).
// Game thread only.
//
// Units: this file's tick, mirrors and shields (command_menu_tick.cpp); the seats' pads' presses and releases
// (menu_input_route); the tactical tint and the wait sound (menu_tint); what they share, the pass in flight
// above all (command_menu_tick_shared.h).
//
// Per seat: every built seat's controller ticks after the game's, in seat order, each
// pass in the seat's own scopes, closed before the next seat's. The pass in flight names its kind and its seat; the
// world slow written in a seat's pass is the smallest of this frame's values so far (player 1's and the earlier
// seats'), so the last pass leaves the minimum over every menu. Every battle pass (this tick, the press route, the
// targeting run) opens the ladder's ButtonsPart with the pass's seat.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::command_menu_tick {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The pass in flight: 0 none, 1 the game's controller's tick, 2 a seat's controller's, 3 a seat's pad's press or
/// release in that seat's UI. Game thread.
int Pass() noexcept;

/// The seat of the pass in flight (passes 2 and 3), else 0. Game thread.
int PassSeat() noexcept;

/// The game's own calls that give the controller's command menu the UI focus of the focus context that is
/// open (call it inside the seat's): FieldCommandCtrl_SetMenuFocusLayer783(controller, 0) while its layer
/// byte (+0x783) still says the menu layer is up, then FieldCommandCtrl_ResumeHudInput(controller, 0),
/// which turns battle input on and puts the layer back (its layer setter returns early while the byte is
/// set). False if the stage is not installed or a call faulted.
bool RefocusMenu(void* controller) noexcept;

/// The controller's battle input flag (+0x25C) is on.
bool BattleInputOn(const void* controller) noexcept;

/// `seat`'s controller ticks now and its pad's presses reach it: it is set up, and a battle is on (either controller's
/// battle HUD mode) or its menu opens outside battle (the leader-type answer and the menu-allowed follow are placed).
/// Game thread.
bool SeatTicks(int seat) noexcept;

/// While one is alive, a game call on a seat's controller outside this unit's passes (the lifecycle's Setup, ResetAll,
/// the Members panel rebuild and the refocus, battle/command_menu) gets the seat's member as the controller's leader
/// type outside battle, as the passes do; open it inside the seat's leader-type scope. Nested ones count. Game thread.
class OwnMemberScope
{
public:
    OwnMemberScope() noexcept;
    ~OwnMemberScope();
    OwnMemberScope(const OwnMemberScope&)            = delete;
    OwnMemberScope& operator=(const OwnMemberScope&) = delete;
};

/// Whether an open command menu slows the world, greys the screen and muffles the sound (the game's behaviour). The
/// settings file's value is set at install, before any redirect is placed; SetMenuSlow switches it afterwards (the
/// Co-op page and the console's `menu-slow on|off`), from the next controller tick, where the grey and the muffle of
/// any waiting menu follow (battle/menu_tint). `why` names the source in the log line. Any thread.
bool MenuSlow() noexcept;
void SetMenuSlow(bool slows, const wchar_t* why) noexcept;

}  // namespace lss::battle::command_menu_tick
