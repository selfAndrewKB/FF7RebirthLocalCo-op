#pragma once

// The fields of the game's actors that several modules read, one reader each.
//
// A controller's pawn, camera manager, leader, trigger and leader-action component; a pawn's controller; a character's controllers, move state and AI; an
// AI's brain and its state; an actor's tags; a widget's slot; a camera manager's owner, view target, field operator and
// camera actor; the field root's game state and trigger manager. Each is one guarded read (null for a null object or a faulting read), as
// the modules made before; any thread may call them, and what they return belongs to the thread that owns the object.

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/level_state.h"
#include "facts/menu.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded_read.h"

namespace lss::players::actor_fields {

// --- a player controller -------------------------------------------------------------------------------------------

/// The pawn a controller possesses, or null.
inline void* Pawn(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerPawn);
}

/// A player controller's camera manager, or null.
inline void* CameraManager(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerCameraManager);
}

/// The character a controller leads with, or null.
inline void* LeaderCharacter(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerLeader);
}

/// A controller's trigger actor (its overlap tracker's owner), or null.
inline void* Trigger(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerTrigger);
}

/// A controller's leader-action component, or null.
inline void* LeaderActions(const void* controller) noexcept
{
    return platform::ReadPointer(controller, facts::kControllerLeaderActions);
}

// --- a pawn, a character -----------------------------------------------------------------------------------------------

/// The controller that possesses a pawn, or null.
inline void* Controller(const void* pawn) noexcept
{
    return platform::ReadPointer(pawn, facts::kPawnController);
}

/// The controller a character asked for (the one a possession hands it to), or null.
inline void* RequestedController(const void* character) noexcept
{
    return platform::ReadPointer(character, facts::kCharacterRequestedController);
}

/// The controller a character's input goes to now (a player's, or its AI's), or null.
inline void* EffectiveController(const void* character) noexcept
{
    return platform::ReadPointer(character, facts::kCharacterEffectiveController);
}

/// The AI controller a character keeps while a player possesses it, or null.
inline void* RetainedAi(const void* character) noexcept
{
    return platform::ReadPointer(character, facts::kCharacterRetainedAi);
}

/// A character's move state, or null.
inline void* MoveState(const void* character) noexcept
{
    return platform::ReadPointer(character, facts::kCharacterMoveState);
}

/// The character a move state belongs to, or null.
inline void* MoveStateCharacter(const void* state) noexcept
{
    return platform::ReadPointer(state, facts::kMoveStateCharacter);
}

/// An actor's tag array's data (FName entries), or null.
inline void* Tags(const void* actor) noexcept
{
    return platform::ReadPointer(actor, facts::kActorTagsData);
}

/// An actor's first tag (a field-action actor's point key), or 0 when it has none.
inline std::uint64_t FirstTag(const void* actor) noexcept
{
    const void* tags = Tags(actor);
    return tags != nullptr && platform::ReadField<std::int32_t>(actor, facts::kActorTagsCount) > 0
               ? platform::ReadField<std::uint64_t>(tags, 0)
               : 0;
}

// --- an AI controller --------------------------------------------------------------------------------------------------

/// An AI controller's active brain, or null.
inline void* ActiveBrain(const void* ai) noexcept
{
    return platform::ReadPointer(ai, facts::kAiActiveBrain);
}

/// A brain's state, or null.
inline void* BrainState(const void* brain) noexcept
{
    return platform::ReadPointer(brain, facts::kBrainState);
}

// --- a camera manager --------------------------------------------------------------------------------------------------

/// The player controller that owns a camera manager, or null.
inline void* CameraOwner(const void* cameraManager) noexcept
{
    return platform::ReadPointer(cameraManager, facts::kCameraManagerPcOwner);
}

/// A camera manager's view target, or null.
inline void* ViewTarget(const void* cameraManager) noexcept
{
    return platform::ReadPointer(cameraManager, facts::kCameraManagerViewTarget);
}

/// A camera manager's field operator, or null.
inline void* FieldOperator(const void* cameraManager) noexcept
{
    return platform::ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
}

/// A camera manager's camera actor, or null.
inline void* EndCameraActor(const void* cameraManager) noexcept
{
    return platform::ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor);
}

// --- a widget, the field root ------------------------------------------------------------------------------------------

/// A widget's panel slot, or null.
inline void* WidgetSlot(const void* widget) noexcept
{
    return platform::ReadPointer(widget, facts::kWidgetSlot);
}

/// The field root's game state (the on/off table's owner), or null.
inline void* RootGameState(const void* root) noexcept
{
    return platform::ReadPointer(root, facts::kRootGameState);
}

/// The field root's trigger manager, or null.
inline void* RootTriggerManager(const void* root) noexcept
{
    return platform::ReadPointer(root, facts::kRootTriggerManager);
}

}  // namespace lss::players::actor_fields
