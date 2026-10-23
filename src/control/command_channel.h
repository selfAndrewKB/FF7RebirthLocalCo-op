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
// (control/runner.h) it shares with the console (control/remote.h owns both). A script with any invalid line, or an
// `abort` (submitted whole, it would drop every line before it), is rejected whole, before anything runs; a dropped
// line fails the script. While a script runs, each reply is appended to its result file at once (and logged), so a
// result survives a `quit` or a crash; the result file's last line is `finished: ok`, `finished: failed` or
// `finished: rejected`.
//
// Scripts already in the inbox when the channel opens were sent to an earlier game session (their sender timed out,
// or the game closed first). They are retired unrun (`finished: skipped`): a stale `quit` would otherwise end the
// next launch at startup.
//
// Game thread only: the console's Tick calls BeforeRun, hands it its replies (Take), then AfterRun.

#include "control/runner.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace lss::control {

class CommandChannel
{
public:
    /// The scripts' client ids: from here up, so they never meet the console's (control/server.h counts from 1).
    static constexpr ClientId kFirstClient = 0x8000'0000u;

    /// Creates the folders next to the loaded DLL; scripts run on `runner`. False (logged) if the folders cannot be
    /// made; the channel then stays closed and its calls do nothing.
    bool Open(Runner& runner) noexcept;

    /// Before the runner's tick: looks for a new script every few ticks while none runs.
    void BeforeRun(std::uint64_t tick);

    /// The reply belongs to a script (kFirstClient and up).
    static bool Owns(ClientId client) noexcept { return client >= kFirstClient; }

    /// One of the running script's replies: written to its result file and the log.
    void Take(const Reply& reply);

    /// After the runner's tick: the script ends once its client has nothing queued or held.
    void AfterRun();

    /// An exception stopped the console: the running script gets its `finished: failed` (its sender waits
    /// for one) and the channel closes.
    void Abandon() noexcept;

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
    void End();

    std::filesystem::path  inbox_;
    std::filesystem::path  done_;
    bool                   open_    = false;
    std::uint64_t          tick_    = 0;
    ClientId               scripts_ = kFirstClient - 1;  // the last client id given: one per script
    Runner*                runner_  = nullptr;
    std::optional<Running> running_;
};

}  // namespace lss::control
