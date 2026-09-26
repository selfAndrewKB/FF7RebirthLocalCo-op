#pragma once

// Player 2 leaves before the game leaves the field, and rejoins once the field is running again
// (RE-p2-retry-reload.md, pass S-RL1).
//
// A retry, a checkpoint reload, loading a save, chapter select and a scripted location jump all
// leave the field and enter it again inside the same world: no map load, so the LoadMap pre-callback
// never runs, and the party characters are destroyed and rebuilt under player 2. Every one of them
// first asks the game mode to leave the field (AEndGameMode_RequestFieldExit_Clear71, which clears
// the "field running" byte GM+0x71) before anything is torn down. Its two calls are redirected: the
// replacement lets player 2 leave (the same leave as before a map load), then runs the game's
// request. A Blueprint caller reaches it by a tail jump; the engine-tick check of the running byte
// covers that one (FieldLeft). The rejoin waits for the field to be running again (FieldReady: the
// entry steps done, the party rebuilt). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::field_exit {

using LeaveFn = void (*)(void* context);

/// `leave` is called (with `context`) when the game asks to leave the field while it runs.
bool Install(const platform::GameImage& image, LeaveFn leave, void* context) noexcept;
void Remove() noexcept;

/// The field is not running (the game is leaving it, or has not entered it yet).
bool FieldLeft() noexcept;

/// The field is running again: the entry steps are done and the party manager is idle (true when
/// this module is not installed).
bool FieldReady() noexcept;

/// Once per engine tick: the field step and running byte logged on change.
void Watch() noexcept;

}  // namespace lss::player_two::field_exit
