#pragma once

// Scour and Scent for players 2 and 3: a press on their own chocobo runs the game's own Scour or Scent for their chocobo,
// their member and their view (research/RE-chocobo-scour-scent-seats.md; the game's code calls the system "chocobo
// craft").
//
// The game has one Scour and Scent session (one manager, the field root's slot): the chocobo's input slot only writes a
// request byte, and the manager's tick in the world tick consumes it and runs the sniff or the dig, its filter, status
// icon and arrows, the dig points' effects and the item, always for the party leader's chocobo. So a player 2's press
// set the byte and player 1's chocobo sniffed. The mod keeps the game's one session and gives it an owner:
// - whose press it was: a request byte that rises in a seat's ride tick (PressWatch) is that seat's; one already set at
//   the manager's tick with nobody named is player 1's;
// - the manager's tick (its one call, a call redirect placed from the game thread) runs inside the owner's primary scope
//   and leader answer while a player 2's or 3's session lasts, so every read of the party leader, the primary
//   controller's coordinator and camera there names the owner's member, chocobo and view;
// - the two row gates in the chocobo's input slot (call redirects) run in the rider's leader answer, so each rider's
//   Scent and Scour rows light for their own chocobo; while another player's session runs they answer no, the game's
//   own dim row, so a second press cannot reach the one session;
// - the sniff action's three reads of the party leader and the field locks' two reads of the leader's chocobo, made
//   outside the manager's tick (the action's states, the end delegate, the dig's callbacks; any thread), answer the
//   owner's member and chocobo while a player 2's or 3's session lasts;
// - the session ends (no request, smell mode off, every field lock it took given back, the item's timers run out, for a
//   second; at once when the owner leaves the game or stops riding; after ten quiet seconds whatever the locks say) and
//   the owner is free again;
// - before each view's render the session's filter, status icon and arrows are made transparent in every view but the
//   view of the player the tick runs as (rides/treasure_hint: the owner during a session).
// The sniff, the dig, the effects, the item, the game-wide field locks and the messages are the game's. Logged: Info at
// install, per press and per session's end; Debug per scope, per answer (once a session), per rows dimmed.

namespace lss::platform {
class GameImage;
}

namespace lss::rides::scour_scent {

bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// The Scour and Scent manager now (the field root's slot), or null; read again at each use (it goes with the field).
void* Manager() noexcept;

/// The manager's session struct (the icon set, the dig, the sniff), or null for a null manager.
void* Session(const void* manager) noexcept;

/// Around a seat's ride tick (its chocobo's input slot runs inside): a Scent or Scour request that rises during it was
/// that seat's press. Game thread.
class PressWatch
{
public:
    explicit PressWatch(int seat) noexcept;
    ~PressWatch();
    PressWatch(const PressWatch&)            = delete;
    PressWatch& operator=(const PressWatch&) = delete;

private:
    int seat_ = -1;  // the seat watched (-1: nothing to name)
};

/// Hides, for the render pass about to run, the session's filter, status icon and arrows unless the tick runs as the seat (0 player 1).
/// Restores first. Game thread.
void ShowOnly(int seat) noexcept;

/// Every widget ShowOnly hid back to its own opacity.
void Restore() noexcept;

}  // namespace lss::rides::scour_scent
