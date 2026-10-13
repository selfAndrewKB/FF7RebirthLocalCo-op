#pragma once

// Logging only: what becomes of player 2's controller after a leave (RE-p2-controller-after-leave.md, S-PC2LEAK,
// "Logging step").
//
// The game's force-feedback registry grows by one per leave, so old player 2 controllers are never freed. The leave
// destroys each one by the engine's own path (RemoveLocalPlayer, stock UE 4.26), and the garbage collector should
// then free it; something reachable must hold it. This watch names what, read-only:
//   A. around RemoveLocalPlayer, in the leave's own step: the local player still names the controller, its role, and
//      afterwards whether Destroy ran, the PendingKill flag, and whether it left its level's actor list;
//   B. after the leave, each second for 2 minutes and then every 10 s: whether the controller, its PlayerInput and
//      the 4 leftover actors (destroyed in the same step, so they show when the collector has run) are still in
//      their object slots; each one's end is logged once;
//   C. once the leftover actors are gone and the controller is not: its internal flags (RootSet, ClusterRoot...)
//      and one walk of the object array listing every live object whose outer chain holds the controller (the
//      outer reference is the kind the collector never clears). The walk costs ~20-25 ms, once per leave.
// Every line is Info: a few per leave.

#include "player_two/leftover_actors.h"

namespace lss::player_two::leave_watch {

/// Game thread, in the leave's step, right before RemoveLocalPlayer.
void BeforeRemove(void* localPlayer, void* controller, const leftover_actors::Actors& actors) noexcept;

/// Game thread, right after it; `removed`: the engine no longer lists player 2.
void AfterRemove(bool removed) noexcept;

/// Game thread, each tick: follows the last leave's objects (B, then C once).
void Tick() noexcept;

}  // namespace lss::player_two::leave_watch
