#pragma once

// A third full-size view, for the multi-monitor measurement (the multi-monitor-support branch).
//
// The question (the user, 2026-10-20): what three full 16:9 views cost on a high-end PC. The engine's own
// levers give three views without any layout code of the mod's: a third local player (UGameplayStatics::
// CreatePlayer, as player 2's join), and UGameMapsSettings::ThreePlayerSplitscreenLayout set to Vertical, which
// UGameViewportClient lays out as three equal full-height columns (UE 4.26 GameViewportClient.cpp,
// ThreePlayer_Vertical: 1/3 wide, 1.0 high, at 0, 1/3 and 2/3). A window 3 x 2560 wide makes each column
// 2560x1440 (r.SetRes 7680x1440w: the console path resizes the window without the settings path's clamp to the
// primary display; GameEngine.cpp DetermineGameWindowResolution, SceneViewport.cpp ResizeFrame).
//
// The third player is a measurement stand-in, not a player: it has no pad, no party member and no HUD pass.
// Its camera manager is created through player 2's own join path (player_two/own_controller.h makes the End
// camera manager's BeginPlay safe for a second player; it applies to any later one) and is aimed at player 1's
// camera actor with the game's own SetViewTarget, so the third column shows player 1's view from player 1's
// camera: the same scene cost as a player's view. Re-aimed whenever player 1's view target changes (cutscenes).
//
// Needs player 2 in. Removed before player 2 leaves and before any map load (mod.cpp), and by itself once
// player 2 is out. The split FOV rule is left to the stock one while it is in (the halves rule assumes two).
// Game thread only. Logged at Info on change, at Debug every 5 s.

#include <cstdint>

namespace lss::views::third_view {

/// `removeLocalPlayer`: the live address of UGameInstance::RemoveLocalPlayer, verified at startup.
void Install(std::uintptr_t removeLocalPlayer) noexcept;

/// Adds the third local player, sets the three-column layout and aims its camera. False (logged) if it cannot.
bool Add() noexcept;

/// Removes it and puts the layout setting back. `why` is logged.
void Remove(const wchar_t* why) noexcept;

/// Each tick after the session's: leaves with player 2, follows player 1's view target, the Debug status.
void Tick(bool playerTwoIn) noexcept;

bool In() noexcept;

}  // namespace lss::views::third_view
