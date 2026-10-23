#pragma once

// Control scripts from files: a way to drive the mod, and through it the game, without keys or the game window's
// focus.
//
// The channel's folder sits beside the mod's `dlls` folder:
//   Mods/LocalSplitScreen/control/inbox/   scripts to run (*.txt; control/script.h has the language, `help` the
//                                          commands)
//   Mods/LocalSplitScreen/control/done/    each script once taken, and its <name>.result.txt
//
// A writer creates the script under another extension (say `.tmp`) and renames it to `.txt`, so the channel never
// reads a half-written file. Scripts run one at a time, lowest file name first, each as one client of the runner
// (control/runner.h). A script with any invalid line, or an `abort` (submitted whole, it would drop every line before
// it), is rejected whole, before anything runs; a dropped line fails the script. While a script runs,
// each reply is appended to its result file at once (and logged), so a result survives a `quit` or a crash; the
// result file's last line is `finished: ok`, `finished: failed` or `finished: rejected`.
//
// Scripts already in the inbox when the channel opens were sent to an earlier game session (their sender timed out,
// or the game closed first). They are retired unrun (`finished: skipped`): a stale `quit` would otherwise end the
// next launch at startup.
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
    /// Creates the folders next to the loaded DLL and the runner over the mod's registry, with `holds` for the
    /// scripts' stick holds. Every command is registered by then (the registry's refusals are logged here). False
    /// (logged) if the folders cannot be made; the channel then stays closed and Tick does nothing.
    bool Open(Holds& holds) noexcept;

    /// Runs the current script's next tick, or looks for a new script every few ticks.
    void Tick() noexcept;

private:
    struct Running
    {
        ClientId              client = 0;
        std::filesystem::path results;
        std::wstring          name;
        bool                  failed = false;
    };

    bool RetireLeftovers();
    void StartNext();
    void Append(const std::filesystem::path& file, const std::wstring& line) noexcept;
    void Report(const std::vector<Reply>& replies);
    void End();

    std::filesystem::path  inbox_;
    std::filesystem::path  done_;
    bool                   open_    = false;
    std::uint64_t          tick_    = 0;
    ClientId               scripts_ = 0;  // the last client id given: one per script
    std::optional<Runner>  runner_;
    std::optional<Running> running_;
};

}  // namespace lss::control
