#pragma once

// Logs, read-only, what the member choice and the hand-over need (RE-p2-party-order.md §6,
// RE-p2-member-handover.md §7); a dev tool (dev/dev_tools.h installs and ticks it; a player's build has the empty
// stubs below, and keeps the party-order Info line through party_member::ChooseSlot's own log at the session's
// party-change check):
//   - the deployed party's order (the battle-member list the party panel shows), the field leader and
//     the member the 2nd-slot rule would give player 2, beside the member player 2 holds: at each game
//     party change and whenever the watch sees the order change;
//   - each deployed member's AI state and brain (FOLLOW, LEAD, STAY, DO_NOTHING; base or script
//     brain), on change, with how long the last one lasted and whether the member moves while not
//     following;
//   - at a take and a give-back (and 1 s after each): player 2's member's AI brain stack and pause
//     mask, its anim instance's speed and rotation modifiers, and its state's catch-up fields; while
//     held, those on change, and player 2's member's speed beside player 1's.
// Changes nothing. Game thread only; inert unless player 2 is in.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::session::handover_watch {

#if LSS_DEV

/// Finds the party reads; logged either way. False if a check failed (nothing is logged then).
bool Install(const platform::GameImage& image) noexcept;

/// The party order and the 2nd-slot rule's member, beside `playerTwoMember` (null if none). `why`
/// says what prompted it (a string literal).
void PartyOrder(const wchar_t* why, const void* playerTwoMember) noexcept;

/// `member`'s AI, modifiers and state fields now, and again 1 s later. `when` is a string literal.
void Snapshot(const wchar_t* when, void* member) noexcept;

/// Each engine tick, after the session's.
void Tick() noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void PartyOrder(const wchar_t*, const void*) noexcept {}
inline void Snapshot(const wchar_t*, void*) noexcept {}
inline void Tick() noexcept {}

#endif

}  // namespace lss::session::handover_watch
