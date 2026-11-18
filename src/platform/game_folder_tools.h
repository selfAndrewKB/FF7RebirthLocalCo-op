#pragma once

// The other tools beside the game: a line at start-up naming every DLL in the game's binary folder that is not the
// game's own or UE4SS's (a proxy DLL such as winmm.dll, dinput8.dll, dsound.dll, version.dll, dxgi.dll is how
// injectors, ASI loaders and shader tools load), with the description and maker from its version information, and every
// .asi file in that folder and in its scripts and plugins folders (where ASI loaders look). An ultrawide fix or a
// shader tool changes what the mod sees (the HUD's layout image, the cameras' shape, the frame); a player's log then
// names it.

namespace lss::platform {

/// Reads the game's folder and logs one line. Any thread; touches no game state.
void LogGameFolderTools() noexcept;

}  // namespace lss::platform
