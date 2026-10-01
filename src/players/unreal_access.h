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
#include <optional>
#include <string>

namespace RC::Unreal {
class UFunction;
}

namespace lss::players::unreal {

using RC::Unreal::UObject;

/// UObject::ProcessEvent inside a structured-exception guard (platform/guarded.h): false if the
/// engine raised an exception.
bool GuardedProcessEvent(UObject* target, RC::Unreal::UFunction* function, void* params) noexcept;

/// The running game instance (FF7R's UEndGameInstance), or null before the game has one.
UObject* GameInstance() noexcept;

/// The game instance's `LocalPlayers` array: the engine's own list of local players.
RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept;

/// The address of a local player's `PlayerController` field, or null if reflection cannot find
/// it or finds it anywhere other than engine::kLocalPlayerControllerOffset.
UObject** ControllerField(UObject* localPlayer) noexcept;

/// UGameMapsSettings::TwoPlayerSplitscreenLayout on the class default object (the value the
/// engine reads every frame), or empty if reflection cannot find it where the engine reads it
/// (engine::kTwoPlayerSplitscreenLayoutOffset, one byte).
std::optional<std::uint8_t> ReadTwoPlayerSplitscreenLayout() noexcept;

/// Writes it; false (writing nothing) under the same conditions.
bool WriteTwoPlayerSplitscreenLayout(std::uint8_t value) noexcept;

/// The offset of ULocalPlayer::AspectRatioAxisConstraint as reflection reports it, if it is the
/// one-byte field the engine reads (engine::kAspectRatioAxisConstraintOffset).
std::optional<std::uint32_t> AspectRatioAxisConstraintOffset() noexcept;

/// UGameplayStatics::CreatePlayer(worldContext, controllerId, bSpawnPlayerController = true): a
/// local player with its own PlayerController. The login path's camera-manager BeginPlay faults for
/// a second player unless player_two/own_controller.h is installed and a join scope is open.
struct CreatedPlayer
{
    bool     called     = false;  // the function was found and the call returned without an exception
    UObject* controller = nullptr;  // the new player's controller, as the call returned it
};
CreatedPlayer CreatePlayerWithController(UObject* worldContext, std::int32_t controllerId) noexcept;

/// How many objects of `className` (and subclasses) exist.
std::size_t CountObjects(const wchar_t* className) noexcept;

/// Calls UGameInstance::RemoveLocalPlayer through the game instance's own vtable, after checking
/// that the entry holds the function at `removeLocalPlayer` (a live address). Returns false,
/// calling nothing, if it does not, or if the call raised an exception.
bool RemoveLocalPlayer(UObject* gameInstance, UObject* localPlayer, std::uintptr_t removeLocalPlayer) noexcept;

/// APlayerController::GetViewportSize: the size in pixels of the whole game viewport (every
/// player's part together), or empty if the function cannot be found or raised an exception.
struct ViewportSize
{
    std::int32_t width  = 0;
    std::int32_t height = 0;
};
std::optional<ViewportSize> ViewportSizeOf(UObject* playerController) noexcept;

/// UGameplayStatics::IsSplitscreenForceDisabled: whether the game viewport's stock
/// bDisableSplitScreenOverride is set (players/single_view.h), or empty if the call failed.
std::optional<bool> SplitscreenForceDisabled(UObject* worldContext) noexcept;

/// UGameplayStatics::SetForceDisableSplitscreen: sets or clears that switch; the engine lays the
/// players out again in the same call. False if it could not be called.
bool SetForceDisableSplitscreen(UObject* worldContext, bool disable) noexcept;

/// UKismetSystemLibrary::GetConsoleVariableIntValue: a console variable's current value, or empty
/// if the function cannot be found or raised an exception.
std::optional<std::int32_t> ConsoleVariableInt(const wchar_t* variable) noexcept;

/// UKismetSystemLibrary::ExecuteConsoleCommand, through the first player's controller: the same
/// path as a command typed into the game's console. Returns false if it could not be called; the
/// engine reports no result, so callers read back what they changed.
bool ExecuteConsoleCommand(const wchar_t* command) noexcept;

/// An FName's text (8 bytes as the engine stores it: comparison index, then number), truncated to 127
/// characters. A name read from game memory can index past the name pool, so the lookup runs under
/// a structured-exception guard; "?" if it faulted.
std::wstring NameText(std::uint64_t name) noexcept;

}  // namespace lss::players::unreal
