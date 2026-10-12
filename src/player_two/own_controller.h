#pragma once

// Player 2's own PlayerController: the camera manager's BeginPlay, made safe for a second player.
//
// When a player joins during play, the engine spawns its controller and camera manager, and the
// camera manager's BeginPlay (AEndPlayerCameraManager, vtable slot 103) runs inside that spawn.
// For a second player it fails three ways:
//   - its owning controller (PCOwner, +0x228) is not set yet, and SetViewTarget reads through it;
//   - it spawns two actors with fixed names ("EndCameraActor", "CompositeCollisionActor"), and
//     the spawn treats a name that already exists (player 1's) as a fatal error.
// The replacement for that slot, only while a join is under way and only for a camera manager
// whose owner is not player 1's controller:
//   - fills PCOwner from the actor's Owner (+0xE8), the controller that spawned it, which is what
//     the engine itself writes there right after the spawn;
//   - sets the two name globals to None for the length of the call, so the spawn generates unique
//     names. Their only reader is this function, so nothing else ever sees None.
// Player 1's camera manager, and every camera manager outside a join, take the engine's path.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::own_controller {

/// Replaces the camera manager's BeginPlay slot after checking it and the name globals.
bool Install(const platform::GameImage& image) noexcept;

/// Puts the engine's function back (at mod uninstall).
void Remove() noexcept;

/// True if a join can run now: the slot is installed and both names are initialised. The second
/// name is set lazily by player 1's camera manager; a join before that would have the engine set
/// it during player 2's call, after the swap, and collide.
bool Ready() noexcept;

/// True if player 1's controller and its camera manager carry the native End classes' vtables.
/// The replacement recognises player 2's controller by that vtable; Blueprint subclasses are
/// expected to share it, and this checks it on the running game before a join relies on it.
bool UsesNativeClasses(const void* playerOneController) noexcept;

/// What the replacement did for the most recent player-2 camera manager.
struct Report
{
    void* cameraManager = nullptr;
    bool  ownerFilled   = false;  // PCOwner was null and was set from Owner
    bool  ownerRefused  = false;  // PCOwner was null but Owner was not a fitting controller
    bool  namesSwapped  = false;
};

/// Game thread: player 1's controller while a join's CreatePlayer runs (player 2's controller is constructed inside
/// it), else null.
void* JoiningPlayerOne() noexcept;

/// Marks a join as under way for its lifetime: only then does the replacement act.
class JoinScope
{
public:
    explicit JoinScope(void* playerOneController) noexcept;
    ~JoinScope();
    JoinScope(const JoinScope&)            = delete;
    JoinScope& operator=(const JoinScope&) = delete;

    Report Result() const noexcept;
};

}  // namespace lss::player_two::own_controller
