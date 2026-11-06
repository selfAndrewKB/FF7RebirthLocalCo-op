#pragma once

// The seats' players, as every wrapper sees them: one generation per seat (players/seats.h).
//
// This is the one record of each seat's game objects (plan/seats.md, the close of step 3): its controller, the member
// it plays and the chocobo it drives, and the reverse lookups from an object the engine hands a wrapper to its seat.
// The session writes the controller and member (Begin at the join, SetMember at the take and the give-back, Withdraw
// and Fault to stop the wrappers, End at the removal) and keeps its own working copy in its Seat; the chocobo comes
// each tick from chocobo_ride through BeginTick. The camera manager and operators are read from the controller each
// time (camera/camera_access.h).
//
// Copies kept elsewhere, each for one activity and each checked (a step-6 audit of a seat's objects starts here):
//   - primary_controller's binding: what it swaps into the game, which reads raw values; unbound at the removal;
//   - vault_camera's operator: refreshed from here each tick for a callback that cannot ask; cleared when inactive;
//   - camera_override's member per seat: cleared when the seat lets go or the session sees the member destroyed;
//   - shared_ride's given member and traversal_holders' climb member: kept with their weak pointers;
//   - own_ride's and chocobo_ride's ride objects: the rides' own, released by the session's leave (rides::own_ride::Release).
// Remake keeps its record as weak pairs (players/seat_objects.h): there only the camera module's copy of a seat's
// controller was checked, and the session's record and the scopes' binding were not. Here End forgets a removed
// controller before the collection frees it, and a member the game destroys is caught by its weak pointer each tick
// (the session's MemberDestroyed; BeginTick's weaks for the wrappers on other threads).
//
// A seat's generation runs from the moment its player has its own controller until the leave begins. While it is
// active, the wrappers act for that seat's objects; the moment it is withdrawn, every wrapper passes straight through
// to the engine for it, so the leave works on stock behaviour only. A fault withdraws it the same way and remembers
// why, so a half-working stage never keeps acting. A fault named without a seat (a scope that lost track of the
// primary controller) withdraws every seat.
//
// Seat 0's controller is player 1's, recorded by every Begin. The names without a seat (Active, PlayerTwoController,
// Member, ...) are player 2's (players::kFirstSeat), kept so the modules not yet per seat read as before
// (plan/seats.md step 3 moves them to the seat-taking names).
//
// Written on the game thread (join, possession, leave); read by the wrappers, which the engine calls on the game
// thread. The values are atomics so a wrapper reached from another thread still reads whole values.

#include "players/seats.h"

#include <cstdint>

namespace lss::players::generation {

/// Starts `seat`'s generation for its controller. Player 1's is recorded (as seat 0's) to tell them apart.
void Begin(int seat, void* playerOneController, void* controller) noexcept;

/// The party member `seat`'s player now drives (null once released).
void SetMember(int seat, void* member) noexcept;

/// Ends `seat`'s generation: from now on every wrapper passes through for it.
void Withdraw(int seat) noexcept;

/// After the seat's player's removal: also forgets its controller and member. The removed controller is freed by the
/// next garbage collection (RE-p2-controller-after-leave.md, S-PC2LEAK2), so no reader may still find it here.
/// Player 1's controller stays: it lives on, and the leave before a world change clears the viewport's split switch
/// through it right after this.
void End(int seat) noexcept;

/// Withdraws `seat`'s generation and records `reason` (a string literal). Only the first reason is kept.
void Fault(int seat, const wchar_t* reason) noexcept;

/// Withdraws every seat's generation with `reason`: for a fault no seat can be named for.
void Fault(const wchar_t* reason) noexcept;

/// `seat`'s first fault since its Begin, or null.
const wchar_t* FaultReason(int seat) noexcept;

/// Records the calling thread as the one the engine ticks the world on. Call from the engine tick.
void SetGameThread() noexcept;

/// The engine tick has recorded the game thread (before its first tick, OnGameThread is false everywhere).
bool GameThreadKnown() noexcept;

/// True on the thread recorded by SetGameThread. Every wrapper acts only there; a call on any other thread passes
/// straight through.
bool OnGameThread() noexcept;

bool  Active(int seat) noexcept;
bool  AnyActive() noexcept;
/// Seat 0: player 1's controller (recorded by every Begin); a seat: its player's.
void* ControllerOf(int seat) noexcept;
void* MemberOf(int seat) noexcept;

/// The seat whose controller `controller` is (1.. while recorded, active or not), 0 for player 1's while a seat is
/// active, -1 for none.
int SeatOfController(const void* controller) noexcept;

/// The seat whose member `member` is, or -1.
int SeatOfMember(const void* member) noexcept;

/// Once per engine tick, first thing, on the game thread: the identities the wrappers compare against on any thread,
/// as the game's weak pointers (0 while there is none, or while the seat's generation is inactive). A member's follows
/// SetMember at once; player 1's pawn is read here; `drivenChocobo[seat]` is chocobo_ride's (the chocobo the seat's
/// player rides or is getting off; index 0 unused).
void BeginTick(const void* const (&drivenChocobo)[players::kMaxPlayers]) noexcept;

/// `seat`'s member, as a weak pointer.
std::uint64_t MemberWeakOf(int seat) noexcept;

/// The seat whose member's weak pointer is `weak` (non-zero), or -1.
int SeatOfMemberWeak(std::uint64_t weak) noexcept;

/// Player 1's possessed pawn this tick, as a weak pointer (0 while no seat is active).
std::uint64_t PlayerOnePawnWeak() noexcept;

/// The chocobo `seat`'s player drives this tick (as given to BeginTick), and its weak pointer.
const void*   ChocoboOf(int seat) noexcept;
std::uint64_t ChocoboWeakOf(int seat) noexcept;

// ---- player 2's, by the names the modules not yet per seat use ----

inline bool Active() noexcept
{
    return Active(players::kFirstSeat);
}
inline void* PlayerOneController() noexcept
{
    return ControllerOf(0);
}
inline void* PlayerTwoController() noexcept
{
    return ControllerOf(players::kFirstSeat);
}
inline void* Member() noexcept
{
    return MemberOf(players::kFirstSeat);
}
inline std::uint64_t MemberWeak() noexcept
{
    return MemberWeakOf(players::kFirstSeat);
}
inline const void* Chocobo() noexcept
{
    return ChocoboOf(players::kFirstSeat);
}
inline std::uint64_t ChocoboWeak() noexcept
{
    return ChocoboWeakOf(players::kFirstSeat);
}
inline const wchar_t* FaultReason() noexcept
{
    return FaultReason(players::kFirstSeat);
}

}  // namespace lss::players::generation
