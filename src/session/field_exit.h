#pragma once

// The seats' players leave before the game leaves the field, and rejoin once the field is running again
// (research/RE-p2-retry-reload.md).
//
// A retry, a checkpoint reload, loading a save, chapter select and a scripted location jump all
// leave the field and enter it again inside the same world: no map load, so the LoadMap pre-callback
// never runs, and the party characters are destroyed and rebuilt under the seats' players. Every one of them
// first asks the game mode to leave the field (AEndGameMode_RequestFieldExit_Clear71, which clears
// the "field running" byte GM+0x71) before anything is torn down. Its entry is redirected
// (platform/entry_redirect.h), so its two calls and the Blueprint native's tail jump all arrive: the
// replacement lets the seats' players leave (the same leave as before a map load), then runs the game's
// request. The engine-tick check of the running byte (FieldLeft) stays as a fallback that logs a
// field exit the entry did not see. The rejoin waits for the field to be running again (FieldReady:
// the entry steps done, the party rebuilt). Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::session::field_exit {

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

}  // namespace lss::session::field_exit
