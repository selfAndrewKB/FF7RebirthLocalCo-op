#pragma once

// The rumble list's state its units share (player_two/rumble_list_two.h): the game's lock and player 2's list, the
// add and the stop into it, the counts, the shared ride's flag and copied keys, player 2's member type and the jump
// box's thread for the sources, and each unit's entry points (the shared-ride copy in rumble_list_copy.cpp, the
// sources outside player 2's scopes in rumble_sources_two.cpp). Internal; not a public API.

#include "player_two/rumble_rules.h"
#include "players/unreal_access.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::rumble_list_two {

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

// Player 2's list: written on the game thread under the game's lock, read by adds on any thread under it.
inline std::atomic<void*> g_listTwo{nullptr};

// A shared ride (player 1 rides the buggy or the bronco, player 2 stands aside): adds and stops on the game's list are
// copied into player 2's list, so player 2's pad plays what player 1's does. Set by the game thread; read anywhere.
inline std::atomic<bool> g_copyOne{false};
// The keys copied and not yet stopped there, under the game's lock (the ride's end stops what is left).
inline rumble_rules::KeySet<16> g_copiedKeys;

// Player 2's member's player type, for the sources on any thread (written each engine tick).
inline std::atomic<int> g_memberType{-1};
// 1.004's jump box: the thread whose inline add takes player 2's list at its list load (0: none). Read by the code at
// the load, under the game's lock, against the running thread's id.
inline std::atomic<std::uint32_t> g_jumpThread{0};
static_assert(sizeof(std::atomic<void*>) == sizeof(void*) && std::atomic<void*>::is_always_lock_free,
              "the load's code reads g_listTwo as a plain pointer");
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

inline thread_local bool t_ownerTwo = false;  // this thread runs one of player 2's rumble sources (OwnerTwo)

struct Counts
{
    std::atomic<unsigned> addsOne{0};      // adds left to the game's list
    std::atomic<unsigned> addsTwo{0};      // adds into player 2's list (game thread, player 2 primary)
    std::atomic<unsigned> zipAddsTwo{0};   // ... from player 2's zip line (worker)
    std::atomic<unsigned> zipAddsOne{0};   // the zip line's adds for anyone else
    std::atomic<unsigned> ticksOne{0};     // the game's list ticked by a component (player 1's)
    std::atomic<unsigned> ticksTwo{0};     // player 2's list ticked by player 2's component
    std::atomic<unsigned> appliedTwo{0};   // ... with changes, played on player 2's controller
    std::atomic<unsigned> skippedOne{0};   // the game's list tick skipped inside player 2's primary scope
    std::atomic<unsigned> refused{0};      // player 2's tick without player 2's primary scope (not played)
    std::atomic<unsigned> stopsOne{0};     // stops on the game's list
    std::atomic<unsigned> stopsTwo{0};     // stops on player 2's list (game thread, player 2 primary)
    std::atomic<unsigned> footTwo{0};      // player 2's chocobo's footsteps run in player 2's scope
    std::atomic<unsigned> footRefused{0};  // ... called on the game thread but not run in player 2's scope
    std::atomic<unsigned> footWorkerTwo{0};  // ... called on a worker (the game's gate drops worker calls)
    std::atomic<unsigned> footOther{0};    // every other chocobo's footsteps (game thread)
    std::atomic<unsigned> footWorkerOther{0};  // ... on a worker
    // The sources run as player 2's (OwnerTwo), by source, and what their adds and stops did.
    std::atomic<unsigned> scooterTwo{0};   // player 2's scooter's slot 1 or 8
    std::atomic<unsigned> glideTwo{0};     // the glide rumble of player 2's chocobo
    std::atomic<unsigned> jumpTwo{0};      // the jump box of player 2's chocobo
    std::atomic<unsigned> driftTwo{0};     // player 2's chocobo's drift (state 1 ticks)
    std::atomic<unsigned> brakeTwo{0};     // ... and brake (phase calls)
    std::atomic<unsigned> ownerAddsTwo{0};   // adds into player 2's list from those sources
    std::atomic<unsigned> ownerStopsTwo{0};  // stops on player 2's list from them (game thread)
    std::atomic<unsigned> ownerStopsLeft{0};  // ... on a worker: left (no game path stops a list there)
    std::atomic<unsigned> sourceWorker{0};   // source calls for player 2 on a thread other than the game thread
    std::atomic<unsigned> actionsOnGame{0};  // drift and brake calls for player 2 on the game thread (expected 0)
    // A shared ride: the game's list copied into player 2's list.
    std::atomic<unsigned> copiesTwo{0};      // adds copied
    std::atomic<unsigned> copyStopsTwo{0};   // stops copied (game thread)
    std::atomic<unsigned> copyStopsLeft{0};  // stops on a worker: not copied
};
inline Counts g_counts;

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

/// Player 2's add, as the game's add does it, into player 2's list. False if player 2 has no list now.
bool AddTwo(std::uint64_t key, const void* params, std::uint8_t flag) noexcept;

/// Player 2's stop, as the game's stop does it, on player 2's list (game thread). False if player 2 has no list now.
bool StopTwo(StopNamedFn original, std::uint64_t key) noexcept;

/// The game's own stop by name (the entry's original), for a stop outside the stop's own redirect.
StopNamedFn StopNamedOriginal() noexcept;

namespace copy {

/// A shared ride's copy of an add to the game's list, into player 2's list (any thread, under the game's lock).
void CopyTwo(std::uint64_t key, const void* params, std::uint8_t flag) noexcept;

/// A shared ride's copy of a stop on the game's list, on player 2's list (game thread only; counted otherwise).
void CopyStopTwo(StopNamedFn original, std::uint64_t key, bool game) noexcept;

}  // namespace copy

namespace sources {

/// Player 2's rumble sources outside player 2's scopes (each optional). Returns the install line's text.
std::wstring Install(const platform::GameImage& image) noexcept;

/// Puts the sources' slots back, then the jump box's entry and 1.004's list load.
void Remove() noexcept;

}  // namespace sources

}  // namespace lss::player_two::rumble_list_two
