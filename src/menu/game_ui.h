#pragma once

// The game's UI pieces the Co-op menu's modules share: the UI system's managers, the main menu's first page, weak
// pointers held in the game's widgets, and FStrings read and copied the game's way. Used by menu/coop_menu,
// menu/coop_page_open and menu/coop_page; found at install, then only read. Game thread.
// Taken from Remake's menu/game_ui. Changed: the UI system is a static object whose qwords are the managers; the weak
// pointers are players/weak_object's 8-byte values.

#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::menu::game_ui {

using AssignFn = void* (*)(void* to, const void* from);

/// False if a function or table is not where the facts say, or the weak pointers cannot be read.
bool Install(const platform::GameImage& image) noexcept;

/// The game's FString copy, for writes inside the caller's own guard.
AssignFn Assign() noexcept;

/// The UI system (the focus-layer pop takes it).
void* System() noexcept;
/// The manager in the UI system's slot `slot`, or null.
void* Manager(std::uint32_t slot) noexcept;

/// `window` is the main menu's first page (UEndMainMenuTopWindow, by its vtable).
bool IsTopWindow(const void* window) noexcept;

/// The object of the weak pointer at `offset` in `object`, or null when it is gone.
void* WeakAt(const void* object, std::uint32_t offset) noexcept;

inline std::uint8_t* At(void* object, std::uint32_t offset) noexcept
{
    return static_cast<std::uint8_t*>(object) + offset;
}

/// The game's FString at `at`, read into the mod's (empty if unreadable).
std::wstring ReadString(const void* at) noexcept;

/// The game's copy of `text` into the FString at `to`. False if it faulted.
bool AssignString(void* to, const std::wstring& text) noexcept;

}  // namespace lss::menu::game_ui
