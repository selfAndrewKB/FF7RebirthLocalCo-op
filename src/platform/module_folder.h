#pragma once

// Where this mod's files are: the folder the loaded DLL sits in (Mods/LocalSplitScreen/dlls), and
// the mod's own folder above it (Mods/LocalSplitScreen); and the flag files a player puts there. Also which loaded
// module an address lies in (a pointer the game holds: Streamline's, a pad library's).

#include <cstdint>
#include <filesystem>
#include <string>

namespace lss::platform {

/// The folder of the loaded DLL, or empty if it cannot be found.
std::filesystem::path DllFolder();

/// The mod's folder (the DLL folder's parent), or empty.
std::filesystem::path ModFolder();

/// The file name (no folder) of the loaded module that holds `address`, or empty if none does. Any thread.
std::wstring ModuleNameAt(std::uintptr_t address) noexcept;

/// A flag file: `name` exists in the mod's folder (a switch a player sets by hand, read where it acts).
bool FlagFile(const wchar_t* name) noexcept;

/// `watch_only.txt`: nothing is installed but the frame-hitch watch, so the game's own frame times can be
/// compared with the mod's. Read once, at the first ask.
bool WatchOnly() noexcept;

}  // namespace lss::platform
