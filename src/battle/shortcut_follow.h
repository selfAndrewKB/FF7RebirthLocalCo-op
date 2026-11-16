#pragma once

// The shortcut tables followed on the other controller (RE-p2-shortcut-refresh.md). Each controller keeps its own copy
// of the shortcut assignments, rebuilt from the save after each change; the menus rebuild the UI system's controller's
// only, item use the executing controller's only. Each rebuild is made again on the other controllers (every seat's
// after the game's, in seat order; the game's and the other seats' after one that ran in a seat's scope), so every
// player's shortcuts follow menu edits and item use. A unit of the seats' command menus
// (battle/command_menu.h), which finds, places and removes it. Game thread.

namespace lss::platform {
class DeferredSites;
class GameImage;
}

namespace lss::battle::shortcut_follow {

/// Finds the rebuild's wrapper, the controller's rebuild and their call sites (each optional: a missing one leaves
/// only that source unfollowed). Keeps the image for the logged return addresses.
void Find(const platform::GameImage& image) noexcept;

/// Places the follows through the controller's placement (each optional). Logged.
void Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept;


/// Puts the follows' entry and calls back (one changed after us is left in place, logged).
void Remove() noexcept;

}  // namespace lss::battle::shortcut_follow
