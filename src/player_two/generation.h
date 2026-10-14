#pragma once

// The current player 2, as every player-2 wrapper sees it.
//
// A "generation" runs from the moment player 2 has its own controller until the leave begins.
// While it is active, the wrappers act for player 2's objects; the moment it is withdrawn, every
// wrapper passes straight through to the engine, so the leave works on stock behaviour only.
// A fault withdraws it the same way and remembers why, so a half-working stage never keeps acting.
//
// Written on the game thread (join, possession, leave); read by the wrappers, which the engine
// calls on the game thread. The values are atomics so a wrapper reached from another thread still
// reads whole values.

namespace lss::player_two::generation {

/// Starts a generation for player 2's controller. Player 1's is recorded to tell the two apart.
void Begin(void* playerOneController, void* playerTwoController) noexcept;

/// The party member player 2 now drives (null once released).
void SetMember(void* member) noexcept;

/// Ends the generation: from now on every wrapper passes through.
void Withdraw() noexcept;

/// After player 2's removal: also forgets the controllers and the member, as before the first join.
/// The removed controller is freed by the next garbage collection (RE-p2-controller-after-leave.md,
/// S-PC2LEAK2), so no reader may still find it here.
void End() noexcept;

/// Withdraws the generation and records `reason` (a string literal). Only the first reason is kept.
void Fault(const wchar_t* reason) noexcept;

/// The first fault since Begin, or null.
const wchar_t* FaultReason() noexcept;

/// Records the calling thread as the one the engine ticks the world on. Call from the engine tick.
void SetGameThread() noexcept;

/// The engine tick has recorded the game thread (before its first tick, OnGameThread is false everywhere).
bool GameThreadKnown() noexcept;

/// True on the thread recorded by SetGameThread. Every player-2 wrapper acts only there; a call
/// on any other thread passes straight through.
bool OnGameThread() noexcept;

bool  Active() noexcept;
void* PlayerOneController() noexcept;
void* PlayerTwoController() noexcept;
void* Member() noexcept;

}  // namespace lss::player_two::generation
