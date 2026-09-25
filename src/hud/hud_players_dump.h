#pragma once

// `dump hud-players`: which player the HUD is built for (RE-p2-hud.md pass 8, P8.5 items 1, 2, 4
// and 5), read through the engine's own reflected functions:
//   - the local players and their controllers;
//   - the order of the world's controller list (UGameplayStatics::GetPlayerController by index),
//     which the compass and quest icons take their first entry of;
//   - the party manager's primary controller;
//   - each HUD root's owning player and local player (UUserWidget::GetOwningPlayer and
//     GetOwningLocalPlayer: the widget's stock PlayerContext, which Square's marker code reads);
//   - every widget class UEndMenuSettings names (its FSoftClassPath members), to match the live
//     root classes to the settings members the creators load;
//   - the HUD's owner, EndUISystem, and the stock player layers (hud/hud_trace.h, LogHudManagers).
// It only reads. Game thread only.

namespace lss::hud {

/// Logs the above; returns the number of lines logged, negative on failure.
int DumpHudPlayers() noexcept;

}  // namespace lss::hud
