#pragma once

// The engine objects the players module works with, reached through UE4SS reflection: guarded calls, the
// game instance and its local players, a reflected static function, a controller's viewport size, an FName's
// text. The console is players/console_access.h; the split-screen settings players/split_settings_access.h; the
// local player's add and remove are in players/local_players.cpp.
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

/// The offset of the parameter `name` in `function`'s frame, if it exists with this size and fits the frame:
/// where an input is written, or a result read, around GuardedProcessEvent.
std::optional<std::int32_t> ParamOffset(RC::Unreal::UFunction* function, const wchar_t* name,
                                        std::int32_t size) noexcept;

/// The running game instance (FF7R's UEndGameInstance), or null before the game has one.
UObject* GameInstance() noexcept;

/// An actor's world location: its RootComponent's world translation (the reflected AActor offset,
/// found once). False if the actor, its root or the field cannot be read. Game thread.
bool ActorLocation(const void* actor, float (&out)[3]) noexcept;

/// The game instance's `LocalPlayers` array: the engine's own list of local players.
RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept;

/// The address of a local player's `PlayerController` field, or null if reflection cannot find
/// it or finds it anywhere other than facts::kLocalPlayerControllerOffset.
UObject** ControllerField(UObject* localPlayer) noexcept;

/// A reflected static function (its path "/Script/Engine.<Class>:<Function>") and the class default object of
/// `classPath` to call it on; empty when either is missing (callers test `function`).
struct StaticCall
{
    RC::Unreal::UFunction* function      = nullptr;
    UObject*               defaultObject = nullptr;
};

inline constexpr const wchar_t* kGameplayStatics     = L"/Script/Engine.GameplayStatics";
inline constexpr const wchar_t* kKismetSystemLibrary = L"/Script/Engine.KismetSystemLibrary";

StaticCall FindStatic(const wchar_t* classPath, const wchar_t* functionPath) noexcept;

/// APlayerController::GetViewportSize: the size in pixels of the whole game viewport (every
/// player's part together), or empty if the function cannot be found or raised an exception.
struct ViewportSize
{
    std::int32_t width  = 0;
    std::int32_t height = 0;
};
std::optional<ViewportSize> ViewportSizeOf(UObject* playerController) noexcept;

/// An FName's text (8 bytes as the engine stores it: comparison index, then number), truncated to 127
/// characters. A name read from game memory can index past the name pool, so the lookup runs under
/// a structured-exception guard; "?" if it faulted.
std::wstring NameText(std::uint64_t name) noexcept;

}  // namespace lss::players::unreal
