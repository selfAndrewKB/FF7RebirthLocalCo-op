#pragma once

// Logs, read-only, the game's Wheelie (SB Scooter) system while player 2 is in (RE-p2-wheelie-ride.md §6; plan,
// "Player 2 rides their own Wheelie"). Logging first: nothing here changes a ride.
//
// Every 10 ticks, from player 1's key-4 vehicle coordinator (`PC+0x5C0`, a TMap<uint8, coordinator>; Costa del Sol's
// parked scooters register with it at BeginPlay), whether or not player 1 rides:
//   - the coordinator: active on player 1 (`PC+0x5B8`), state (`*[+0x230]`: 0 idle, 1 resume, 2 boarding, 3 riding,
//     4 getting off), scooter, rider, its flags, the shared block's requested / idle / parking / colour bytes and scooter
//     count, and the per-rider table's state and named entries (expected empty: the table is dormant, §2);
//   - the scooters in its list: how many are free by the game's own rule (not excluded, not ridden), and the nearest
//     free one's distance to player 2's member;
//   - player 2's key-4 coordinator (expected none) and player 2's member, with whether its player type is one of the
//     four with `AnimSet_Wheelie00` (Cloud, Tifa, Aerith, Yuffie);
//   - the on/off counters 152, 136 and 4 the ride's states change (§4: does the interact path go negative?).
// Logged on each change (Info: rare, real changes). While player 1 rides, once a second (Debug): the merged trigger
// values the scooter's accelerate and brake read, each pad's own triggers, and player 1's scooter's accel/brake as its
// move state wrote them (does pad 2's trigger drive player 1's scooter?). Calls only the game's weak-pointer resolve,
// guarded. Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::wheelie_watch {

/// Checks the vtables and the weak-pointer resolve; logged either way. False if a check failed (nothing is read).
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick.
void Tick() noexcept;

}  // namespace lss::player_two::wheelie_watch
