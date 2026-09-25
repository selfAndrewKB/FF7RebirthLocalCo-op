#pragma once

// Where this mod's files are: the folder the loaded DLL sits in (Mods/LocalSplitScreen/dlls), and
// the mod's own folder above it (Mods/LocalSplitScreen).

#include <filesystem>

namespace lss::platform {

/// The folder of the loaded DLL, or empty if it cannot be found.
std::filesystem::path DllFolder();

/// The mod's folder (the DLL folder's parent), or empty.
std::filesystem::path ModFolder();

}  // namespace lss::platform
