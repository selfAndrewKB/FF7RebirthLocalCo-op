#pragma once

// Logs, read-only, what the member choice and the hand-over need (research/RE-p2-party-order.md,
// RE-p2-member-handover.md); a dev tool (dev/dev_tools.h installs and ticks it; a player's build has the empty
// stubs below, and keeps the party-order Info line through party_member::ChooseSlot's own log at the session's
// party-change check):
//   - the deployed party's order (the battle-member list the party panel shows), the field leader and
//     the member the slot rule would give each seat's player, beside the member that player holds: at each
//     game party change and whenever the watch sees the order change;
//   - each deployed member's AI state and brain (FOLLOW, LEAD, STAY, DO_NOTHING; base or script
//     brain), on change, with how long the last one lasted and whether the member moves while not
//     following;
//   - at a take and a give-back (and 1 s after each): the seat's member's AI brain stack and pause
//     mask, its anim instance's speed and rotation modifiers, and its state's catch-up fields; while
//     held, those on change, and the member's speed beside player 1's.
// Changes nothing. Game thread only; inert unless a seat's player is in.

#include "platform/dev_switch.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::session::handover_watch {

#if LSS_DEV

/// Finds the party reads; logged either way. False if a check failed (nothing is logged then).
bool Install(const platform::GameImage& image) noexcept;

/// The party order and, for every seat whose player is in, the slot rule's member beside the member held. `seat`
/// and `member`: the calling seat's member as the caller holds it (null if none), in place of the record's. `why`
/// says what prompted it (a string literal).
void PartyOrder(const wchar_t* why, int seat, const void* member) noexcept;

/// `member`'s AI, modifiers and state fields now, and again 1 s later. `when` is a string literal.
void Snapshot(const wchar_t* when, void* member) noexcept;

/// Each engine tick, after the session's.
void Tick() noexcept;

#else

inline bool Install(const platform::GameImage&) noexcept { return false; }
inline void PartyOrder(const wchar_t*, int, const void*) noexcept {}
inline void Snapshot(const wchar_t*, void*) noexcept {}
inline void Tick() noexcept {}

#endif

}  // namespace lss::session::handover_watch
