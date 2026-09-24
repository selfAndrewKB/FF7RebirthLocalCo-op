#pragma once

// The engine objects the players module works with, reached through UE4SS reflection.
//
// Reflection finds fields and functions by name at runtime, so these helpers need no hard-coded
// offsets. Where the mod also depends on a field's offset from outside reflection (the
// PlayerController field that RemoveLocalPlayer reads directly), the reflected offset is checked
// against it.

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/UObject.hpp>

#include <cstdint>

namespace lss::players::unreal {

using RC::Unreal::UObject;

/// The running game instance (FF7R's UEndGameInstance), or null before the game has one.
UObject* GameInstance() noexcept;

/// The game instance's `LocalPlayers` array: the engine's own list of local players.
RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept;

/// The address of a local player's `PlayerController` field, or null if reflection cannot find
/// it or finds it anywhere other than engine::kLocalPlayerControllerOffset.
UObject** ControllerField(UObject* localPlayer) noexcept;

/// UGameplayStatics::CreatePlayer(worldContext, controllerId, bSpawnPlayerController = false).
/// No controller is spawned: spawning one runs the GameMode login path, which faults in FF7R.
/// Returns false if the function could not be found, its parameters do not match, or the call
/// raised an exception.
bool CreatePlayerWithoutController(UObject* worldContext, std::int32_t controllerId) noexcept;

/// Calls UGameInstance::RemoveLocalPlayer through the game instance's own vtable, after checking
/// that the entry holds the function at `removeLocalPlayer` (a live address). Returns false,
/// calling nothing, if it does not, or if the call raised an exception.
bool RemoveLocalPlayer(UObject* gameInstance, UObject* localPlayer, std::uintptr_t removeLocalPlayer) noexcept;

}  // namespace lss::players::unreal
