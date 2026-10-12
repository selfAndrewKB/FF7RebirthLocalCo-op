#pragma once

// Control scripts from files: a way to drive the mod, and through it the game, without keys or the
// game window's focus.
//
// The channel's folder sits beside the mod's `dlls` folder:
//   Mods/LocalSplitScreen/control/inbox/   scripts to run (*.txt; control/script.h has the language)
//   Mods/LocalSplitScreen/control/done/    each script once taken, and its <name>.result.txt
//
// A writer creates the script under another extension (say `.tmp`) and renames it to `.txt`, so
// the channel never reads a half-written file. Scripts run one at a time, lowest file name first.
// A script with any invalid line is rejected whole, before anything runs. While a script runs,
// each completed command is appended to its result file at once (and logged), so a result survives
// a `quit` or a crash; the result file's last line is `finished: ok`, `finished: failed` or
// `finished: rejected`.
//
// Scripts already in the inbox when the channel opens were sent to an earlier game session (their
// sender timed out, or the game closed first). They are retired unrun (`finished: skipped`): a stale
// `quit` would otherwise end the next launch at startup.
//
// Game thread only: Tick runs from the engine-tick callback, before the world ticks.

#include "control/runner.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace lss::control {

class CommandChannel
{
public:
    /// Creates the folders next to the loaded DLL. False (logged) if they cannot be made; the
    /// channel then stays closed and Tick does nothing.
    bool Open() noexcept;

    /// Runs the current script's next tick, or looks for a new script every few ticks.
    void Tick(Target& target) noexcept;

private:
    struct Running
    {
        Runner                runner;
        std::filesystem::path results;
        std::wstring          name;
    };

    bool RetireLeftovers();
    void StartNext();
    void Append(const std::filesystem::path& file, const std::wstring& line) noexcept;
    void Report(const std::vector<LineResult>& results);
    void End();

    std::filesystem::path  inbox_;
    std::filesystem::path  done_;
    bool                   open_  = false;
    std::uint64_t          tick_  = 0;
    std::optional<Running> running_;
};

}  // namespace lss::control
