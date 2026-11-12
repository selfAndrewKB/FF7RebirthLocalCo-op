#pragma once

// Logging only: what becomes of a seat's controller after a leave (research/RE-p2-controller-after-leave.md).
//
// The leave destroys a seat's controller by the engine's own path (RemoveLocalPlayer, stock UE 4.26), and the garbage
// collector should then free it; whatever still holds it keeps it alive (the game's force-feedback registry grew by
// one per leave while the controller's PlayerInput held it; leftover_actors releases that one). This watch names
// every holder, read-only, so a new one shows in the log:
//   A. around RemoveLocalPlayer, in the leave's own step: the local player still names the controller, its role, and
//      afterwards whether Destroy ran, the PendingKill flag, and whether it left its level's actor list;
//   B. after the leave, each second for 2 minutes and then every 10 s: whether the controller, its PlayerInput and
//      the 4 leftover actors (destroyed in the same step, so they show when the collector has run) are still in
//      their object slots; each one's end is logged once;
//   C. once the leftover actors are gone and the controller is not: its internal flags (RootSet, ClusterRoot...)
//      and one walk of the object array listing every live object whose outer chain holds the controller (the
//      outer reference is the kind the collector never clears), and every input component whose key-map cache still
//      names the controller's PlayerInput (the never-unloaded level scripts' components are the
//      expected holders). The walk costs ~20-25 ms, once per leave.
// Every line is Info: a few per leave.

#include "session/leftover_actors.h"

namespace lss::session::leave_watch {

/// Game thread, in the leave's step, right before RemoveLocalPlayer.
void BeforeRemove(void* localPlayer, void* controller, const leftover_actors::Actors& actors) noexcept;

/// Game thread, right after it; `removed`: the engine no longer lists the seat's player.
void AfterRemove(bool removed) noexcept;

/// Game thread, each tick: follows the last leave's objects (B, then C once).
void Tick() noexcept;

}  // namespace lss::session::leave_watch
