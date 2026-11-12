#pragma once

// A seat's player going out: every module that holds something of the seat's lets go of it, in one place. The
// session calls it at each kind of leave (asked for, before a map load, and once the player is removed), while the
// seat's scopes still hold, before its generation is withdrawn or ended.
//
// Each module listed takes the seat. Game thread only.

namespace lss::session::seat_out {

/// The camera override's delegates back to the engine's, the target fields, the rumble channels stopped while the
/// seat can still be the primary player, the force-feedback component back on the game's manager.
void LetGo(int seat) noexcept;

}  // namespace lss::session::seat_out
