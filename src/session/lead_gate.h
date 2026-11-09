#pragma once

// Whether a walk or lead the level wrote on player 2's or 3's member is needed finished (the member goes to its AI and
// the player rides along) or only staging (the player keeps it), from the game's own data
// (Rebirth/Documentation/research/RE-walk-gate.md).
//
// A run to a mark (MOVE_TO) or a lead (LEAD) on the level's brains signals its arrival with a trigger (the row's string
// argument 2, or the setting group's slot 2); a held member's move never starts, so a level step waiting on it would
// wait for ever. The move is needed when:
//   1. a waiter: another active level slot's current state lists the arrival trigger in the trigger's scope (Self: the
//      member's own object only; SpecificObject: the trigger's SendObject list; else every active slot), or the
//      trigger row's direct sets enter a state on another object;
//   2. the story's objective marker rides on the member;
//   3. a further move follows in the member's own chain of states before it stands or follows again
//      (session/state_path.h, from the state its arrival leads to; a chain past the limit counts as one).
// A leash on player 1 (an AutoWalk act aimed at the member) would be a fourth signal, but no level writes one.
// Whatever else holds, a move whose step moves on at the member's own armed story box (a trigger that box sends, not
// its arrival) goes to its AI: the walk carries the member into the box on the level's clock (world/story_boxes).
// A move is judged at its first sight and the judgment is logged (one Info line: the trigger, every waiter, the marker,
// the chain, the verdict; again at each verdict change). A needed verdict holds until the setting ends (the member's
// state changes) or the member is free again; a staging verdict is re-read while it lasts (the waiters about every
// 3 s, the marker each call). One judgment is kept per level object. A census that cannot be read whole makes the move
// needed. Reads only, through the game's own row accessors and slot lookups, the slot collection under the game's
// state-trigger lock. Game thread.

#include "session/lead_keep_rules.h"

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::session::lead_gate {

/// Resolves the game's row accessors, slot lookups and lock, registers the `lead.gate` and `lead.path` readings. False
/// (logged) if a check fails: every walk or lead the level writes on a held member is then needed (its AI walks it).
bool Install(const platform::GameImage& image) noexcept;

/// `member` (seat `seat`'s) runs a walk or lead on the level's brains: the verdict from the game's data. Guide when the
/// move cannot be read or the gate is not installed (the AI walks it).
lead_keep_rules::Verdict Judge(int seat, const void* member) noexcept;

/// The level has `member` on a walk or lead of its own (a MOVE_TO or LEAD on the level's brains naming a level object),
/// whatever the verdict and whoever walks it: world/story_boxes takes it as the story's moment for the member's own
/// story boxes. False when not installed or unread. Game thread.
bool OnLevelMove(const void* member) noexcept;

/// `member`'s current walk on the level's brains moves on at a trigger (not its arrival) the box state row `boxStateRow`
/// sends (that state lists it and its Status names it): the walk is the one into that box. No census: two row reads.
/// False when not installed or unread. Game thread.
bool WalksIntoBox(const void* member, std::uint64_t boxStateRow) noexcept;

/// `member`'s current walk was judged a guide for its own story box (OwnBoxAhead): the walk's point (world cm) and the
/// box's layout object. False otherwise. world/run_catch_up asks it just before the guide's hand. Game thread.
bool OwnBoxRun(const void* member, float point[3], std::uint64_t& box) noexcept;

/// Seat `seat`'s member is free of any script again: its last judgment is dropped, so the same setting written again
/// is judged afresh.
void Forget(int seat) noexcept;

/// The last judgment, one line (the reading).
std::wstring Reading() noexcept;

}  // namespace lss::session::lead_gate
