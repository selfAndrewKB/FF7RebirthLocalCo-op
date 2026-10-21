#pragma once

// The dump commands (a dev tool): what the HUD's widgets report about themselves, and which player the HUD is built
// for, through the engine's own reflected functions. Everything here only reads. Game thread only.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::hud {

/// `dump widgets`: for every live UUserWidget that is visible and was arranged, one log line (UWidget::IsVisible,
/// GetDesiredSize, GetCachedGeometry), largest first, capped. Returns how many were listed (negative if the
/// functions cannot be found).
int DumpWidgets() noexcept;

/// `dump slots <Class>`: for each live UUserWidget of that class, walks its WidgetTree (RootWidget,
/// then panels' Slots, nested user widgets' trees, a few levels deep) through reflection and logs
/// every panel slot: its class, its content, and for canvas slots the LayoutData (offsets,
/// anchors, alignment) and bAutoSize. Returns the number of slots logged, negative on failure.
int DumpSlots(const std::wstring& className) noexcept;

/// `dump hud-players`: which player the HUD is built for (RE-p2-hud.md pass 8, P8.5 items 1, 2, 4 and 5, P8.9):
///   - the local players and their controllers;
///   - the order of the world's controller list (UGameplayStatics::GetPlayerController by index),
///     which the compass and quest icons take their first entry of;
///   - the party manager's primary controller;
///   - each HUD root's owning player and local player (UUserWidget::GetOwningPlayer and
///     GetOwningLocalPlayer: the widget's stock PlayerContext, which Square's marker code reads);
///   - every widget class UEndMenuSettings names (its FSoftClassPath members), to match the live
///     root classes to the settings members the creators load;
///   - the HUD's owner, EndUISystem (its field command controller's weak widgets and stored controller, the compass
///     holder's widget, the setup flags) and the stock per-player layer geometry each local player would get.
/// Returns the number of lines logged, negative on failure.
int DumpHudPlayers(const platform::GameImage& image) noexcept;

}  // namespace lss::hud
