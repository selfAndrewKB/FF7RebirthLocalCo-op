#pragma once

// The module table (modules.cpp): every module the entry point installs after the session, in install order, each
// with its Remove or none. The entry point (mod.cpp) installs the table in order and removes it in reverse; each
// module logs its own install line, and one whose check fails stands aside alone. The order constraints the modules
// carry (climb_context before traversal_camera, the view point before the streaming views, the overlay layout
// before the layout scale, the opacity setter before the HUD modules that read it) are pinned by static_asserts
// beside the table: the build fails if a row moves past one.
//
// Outside the table, in the entry point: what every module relies on (the renderer, the weak pointers, the
// in-battle test, the session and its stages) before it; the dev tools, the engine callbacks and the field-exit
// catch (they take the session or a callback of the mod's) after it.

namespace lss::platform {
class GameImage;
}

namespace lss::modules {

/// Installs every module in table order.
void Install(const platform::GameImage& image) noexcept;

/// Removes every module that has a Remove, in reverse table order. The rest stay installed: the DLL is pinned,
/// so their code stays valid.
void Remove() noexcept;

}  // namespace lss::modules
