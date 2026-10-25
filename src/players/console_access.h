#pragma once

// The game's console through UKismetSystemLibrary's reflected functions: a console variable's value, a variable
// set, a command run. The same path as a command typed into the game's console; the engine reports no result
// from a command, so callers read back what they changed. The backing of the held console settings
// (players/held_setting.h over players/split_screen_settings.h). Game thread.

#include "platform/dev_switch.h"

#include <cstdint>
#include <optional>

#if LSS_DEV
#include <functional>
#endif

namespace lss::players::console {

/// UKismetSystemLibrary::GetConsoleVariableIntValue: a console variable's current value, or empty
/// if the function cannot be found or raised an exception.
std::optional<std::int32_t> VariableInt(const wchar_t* variable) noexcept;

/// UKismetSystemLibrary::GetConsoleVariableFloatValue, as VariableInt.
std::optional<float> VariableFloat(const wchar_t* variable) noexcept;

/// "<variable> <value>" through Execute (a players::SettingAccess write). False if the command could
/// not be run; whether it took is read back by the caller.
bool SetVariableInt(const wchar_t* variable, std::int32_t value) noexcept;

/// UKismetSystemLibrary::ExecuteConsoleCommand, through the first player's controller: the same
/// path as a command typed into the game's console. Returns false if it could not be called; the
/// engine reports no result, so callers read back what they changed.
bool Execute(const wchar_t* command) noexcept;

#if LSS_DEV
/// The `console` and `quit` commands. `furtherIn` tells quit whether a further player is still in (it refuses then); a
/// function rather than the session, which sits above this layer. The watch-only start registers these alone.
void RegisterCommands(std::function<bool()> furtherIn);
#endif

}  // namespace lss::players::console
