#pragma once

// The rumble list's state its units share (feedback/traversal_rumble.h): the game's lock and each seat's list, the add
// and the stop into one, the counts, the shared ride's flags and copied keys, each seat's member type and the jump
// box's route for the sources, and each unit's entry points (the shared-ride copy in traversal_rumble_copy.cpp, the
// sources outside the seats' scopes in traversal_rumble_sources.cpp). Internal; not a public API.
//
// Per seat (players/seats.h): the list, the shared ride's copy flag and copied keys, the member type and the counts are
// arrays indexed by the seat (index 0, player 1's: unused, except the counts of the game's own list, kept there). The
// owner's list pointer is shared game state: a seat's stop swaps its list in and out under the game's lock, one seat at
// a time (StopOnSeat). The jump box's route names one thread and one list at a time.

#include "feedback/traversal_rumble_code.h"
#include "feedback/traversal_rumble_rules.h"
#include "players/seats.h"
#include "players/unreal_access.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::feedback::traversal_rumble {

constexpr std::uint32_t kListEntries    = 0x8;    // TArray of 48-byte entries
constexpr std::uint32_t kArrayNum       = 0x8;    // a TArray's Num, after its data pointer
constexpr std::uint32_t kMoveCharacter  = 0x450;  // the move's character, an FWeakObjectPtr
constexpr std::uint32_t kOwnerList      = 0x3B0;  // the owner's list pointer

using AddOrUpdateFn = void (*)(void* list, std::uint64_t key, const void* params, std::uint8_t flag);
using StopNamedFn   = void (*)(std::uint64_t key);

inline std::uintptr_t    g_listOwner   = 0;        // the global holding the list's owner
inline AddOrUpdateFn     g_addOrUpdate = nullptr;
inline CRITICAL_SECTION* g_lock        = nullptr;
inline bool              g_installed   = false;

// Each seat's list, by seat: written on the game thread under the game's lock, read by adds on any thread under it.
inline std::atomic<void*> g_seatList[players::kMaxPlayers]{};

// A shared ride (player 1 rides the buggy or the bronco, every seat stands aside): adds and stops on the game's list
// are copied into each seat's list whose flag is set (the seat has its own force-feedback manager), so its pad plays
// what player 1's does. Set by the game thread; read anywhere.
inline std::atomic<bool> g_copyOne[players::kMaxPlayers]{};
// The keys copied into a seat's list and not yet stopped there, under the game's lock (the ride's end stops what is
// left).
inline traversal_rumble_rules::KeySet<16> g_copiedKeys[players::kMaxPlayers];

/// Some seat's list takes the shared ride's copies now.
inline bool AnyCopy() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_copyOne[seat].load(std::memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

// Each seat's member's player type, for the sources on any thread (written each engine tick; -1 none).
inline std::atomic<int> g_memberType[players::kMaxPlayers] = {-1, -1, -1};
// 1.004's jump box: one route per landing in flight (traversal_rumble_code.h): the thread whose inline add takes a
// seat's list at its list load (0: free), and that seat's list (null: none). Read by the code at the load, under the
// game's lock, against the running thread's id; a route claims a free slot and sets its list under the game's lock, a
// list's drop clears it from every slot under it. Two chocobos landing on two animation workers at once each name their
// own slot (RE-seats-step6-camera-rumble.md §2).
inline traversal_rumble_code::JumpSlots g_jumpSlots{};

// The seat one of whose rumble sources this thread runs (SeatOwner), or 0.
inline thread_local int t_ownerSeat = 0;

struct Counts
{
    std::atomic<unsigned> addsOne{0};      // adds left to the game's list ([0])
    std::atomic<unsigned> seatAdds{0};     // adds into the seat's list (game thread, the seat primary)
    std::atomic<unsigned> seatZipAdds{0};  // ... from the seat's zip line (worker)
    std::atomic<unsigned> zipAddsOne{0};   // the zip line's adds for anyone else ([0])
    std::atomic<unsigned> ticksOne{0};     // the game's list ticked by a component (player 1's; [0])
    std::atomic<unsigned> seatTicks{0};    // the seat's list ticked by the seat's component
    std::atomic<unsigned> seatApplied{0};  // ... with changes, played on the seat's controller
    std::atomic<unsigned> skippedOne{0};   // the game's list tick skipped inside the seat's primary scope
    std::atomic<unsigned> refused{0};      // the seat's tick without the seat's primary scope (not played)
    std::atomic<unsigned> stopsOne{0};     // stops on the game's list ([0])
    std::atomic<unsigned> seatStops{0};    // stops on the seat's list (game thread, the seat primary)
    std::atomic<unsigned> seatFoot{0};     // the seat's chocobo's footsteps run in the seat's scope
    std::atomic<unsigned> footRefused{0};  // ... called on the game thread but not run in the seat's scope
    std::atomic<unsigned> seatFootWorker{0};  // ... called on a worker (the game's gate drops worker calls)
    std::atomic<unsigned> footOther{0};    // every other chocobo's footsteps (game thread; [0])
    std::atomic<unsigned> footWorkerOther{0};  // ... on a worker ([0])
    // The sources run as the seat's (SeatOwner), by source, and what their adds and stops did.
    std::atomic<unsigned> seatScooter{0};  // the seat's scooter's slot 1 or 8
    std::atomic<unsigned> seatGlide{0};    // the glide rumble of the seat's chocobo
    std::atomic<unsigned> seatJump{0};     // the jump box of the seat's chocobo
    std::atomic<unsigned> seatDrift{0};    // the seat's chocobo's drift (state 1 ticks)
    std::atomic<unsigned> seatBrake{0};    // ... and brake (phase calls)
    std::atomic<unsigned> seatOwnerAdds{0};  // adds into the seat's list from those sources
    std::atomic<unsigned> seatOwnerStops{0};  // stops on the seat's list from them (game thread)
    std::atomic<unsigned> ownerStopsLeft{0};  // ... on a worker: left (no game path stops a list there)
    std::atomic<unsigned> sourceWorker{0};   // source calls for the seat on a thread other than the game thread
    std::atomic<unsigned> actionsOnGame{0};  // drift and brake calls for the seat on the game thread (expected 0)
    // A shared ride: the game's list copied into the seat's list.
    std::atomic<unsigned> seatCopies{0};     // adds copied
    std::atomic<unsigned> seatCopyStops{0};  // stops copied (game thread)
    std::atomic<unsigned> copyStopsLeft{0};  // stops on a worker: not copied
};
// By seat; index 0 holds the counts of the game's own list (the fields marked [0]).
inline Counts g_counts[players::kMaxPlayers];

inline std::wstring KeyText(std::uint64_t key) noexcept
{
    return players::unreal::NameText(key);
}

class ListLock
{
public:
    ListLock() noexcept { EnterCriticalSection(g_lock); }
    ~ListLock() { LeaveCriticalSection(g_lock); }
    ListLock(const ListLock&)            = delete;
    ListLock& operator=(const ListLock&) = delete;
};

/// `seat`'s add, as the game's add does it, into the seat's list. False if the seat has no list now.
bool AddToSeat(int seat, std::uint64_t key, const void* params, std::uint8_t flag) noexcept;

/// `seat`'s stop, as the game's stop does it, on the seat's list (game thread). False if the seat has no list now.
bool StopOnSeat(int seat, StopNamedFn original, std::uint64_t key) noexcept;

/// The game's own stop by name (the entry's original), for a stop outside the stop's own redirect.
StopNamedFn StopNamedOriginal() noexcept;

namespace copy {

/// A shared ride's copy of an add to the game's list, into each copying seat's list in seat order (any thread, under
/// the game's lock).
void CopyToSeats(std::uint64_t key, const void* params, std::uint8_t flag) noexcept;

/// A shared ride's copy of a stop on the game's list, on each copying seat's list in seat order (game thread only;
/// counted otherwise).
void CopyStopToSeats(StopNamedFn original, std::uint64_t key, bool game) noexcept;

}  // namespace copy

namespace sources {

/// The seats' rumble sources outside their scopes (each optional). Returns the install line's text.
std::wstring Install(const platform::GameImage& image) noexcept;

/// Puts the sources' slots back, then the jump box's entry and 1.004's list load.
void Remove() noexcept;

}  // namespace sources

}  // namespace lss::feedback::traversal_rumble
