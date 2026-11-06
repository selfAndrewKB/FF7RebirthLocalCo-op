#pragma once

// The mirror views: a local player of the mod's in every column no player has, showing player 1's picture, so the
// held columns are never black (plan/seats.md step 5; the user's columns rule of 2026-10-23: a third column only
// while the three-player monitor mode is on, and then player 1's picture in an empty one).
//
// The engine's own levers, as views/third_view (the multi-monitor measurement, 2026-10-20) proved them: a local
// player through UGameplayStatics::CreatePlayer (players/local_players.h), inside a seat's join scope
// (session/own_controller.h makes its End camera manager's BeginPlay safe, naming no seat: nothing a join
// builds for a player goes to a mirror), whose camera manager is aimed at player 1's view target with the game's own
// SetViewTarget (camera/view_target.h) and re-aimed whenever player 1's changes (cutscenes). The engine lays
// the columns out and renders the mirror's view as a player's (its own view family). A mirror has no pad (its
// controller id is after the seats', players::IsMirrorId: no seat's HUD, no seat's pad), no member, no HUD pass of its
// own: its view draws player 1's HUD (hud/passes/second_image.h).
//
// The mirrors follow the players: a seat's join takes every mirror out right before its CreatePlayer (the session's
// clearMirrors) and they come back after it on the same tick, so the seat's column comes before theirs. A mirror
// needs no member and no field: it is added one per tick whenever player 1's controller is the End class and its
// camera manager is ready for a join, whatever the game shows (the title, a loading screen, a movie, a menu), unless
// the caller names a hold; and again if the engine drops it. The mirrors stay in through a field exit inside the
// world (a save load from the title or the menu, a retry, chapter select: the loading screen is then drawn in every
// column, as Remake's is) and leave only before a real map load (a map load would spawn their controllers again
// outside a join; research/RE-mirror-loads.md). The user's ask of 2026-10-26: the title, the loading screens and the
// movies in every column, as Remake does it or better.
// Changed from Remake's views/mirror_player: the camera by SetViewTarget (Rebirth's camera manager honours it), the
// mirrors kept through the field exits (Remake refills a column on the leave's tick), the controller ids after the
// seats' (step 4 keys the HUD by the id).
//
// Wanted while more columns are held than players are in (views/columns.h). Game thread only. Logged at Info on each
// add and remove, at Debug for the status.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::views::mirror_player {

/// Finds the live address of UGameInstance::RemoveLocalPlayer (a startup fact, verified before any install).
void Install(const platform::GameImage& image) noexcept;

/// Each engine tick after the session's: removes the mirrors beyond `wanted`, the highest column first, and adds one
/// toward it unless `hold()` names a reason (asked only when an add is due; null when none); every mirror's camera
/// follows player 1's view target.
void Tick(int wanted, const wchar_t* (*hold)() noexcept) noexcept;

/// Takes every mirror out now (before a seat's CreatePlayer, before a map load). `why` is logged. True when no
/// mirror is left in the engine's list.
bool Clear(const wchar_t* why) noexcept;

/// The mirrors in the engine's list.
int Count() noexcept;

/// The `mirror-view.state` reading: wanted, in which columns with which ids, why one waits.
std::wstring Reading();

}  // namespace lss::views::mirror_player
