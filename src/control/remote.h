#pragma once

// The console: the socket (control/server.h) on 127.0.0.1:kPort and the runner over the mod's registry, joined on the
// game thread. A command per line in, a line per reply out (the runner's ConsoleText); each connection is its own
// client, so a script's queue, waits and holds are its own, and a client that goes away drops its queue and releases
// its holds (a script waits out its holds before it ends).
//
// Open after every module has registered its commands (the registry's refusals are logged then); Tick from the engine
// tick, before the session's; Close before the dev tools go (it joins the socket's thread).
//
// Driving it: `python tools/remote.py shell` (or `-c "<line>"`, `run <file>`, `status`); the regression harness
// (tools/regression/run.py) and tools/relaunch.ps1 go through it.
//
// Taken from Remake's control/remote. Changed: the port (Remake's plus one, so both games' consoles can
// listen at once); a reply's log level read from its kind and command, not its text, and notes at Info (they mark the
// log); `log-level` stays the platform's (platform::RegisterLogCommands); no `tick` reading. The file inbox it ran
// beside for a row of the port is retired: scripts are console clients now.

#include "control/runner.h"
#include "control/server.h"

#include <cstdint>
#include <optional>

namespace lss::control {

class Remote
{
public:
    static constexpr std::uint16_t kPort = 4419;

    /// The runner over the registry with `holds` for the scripts' stick holds, then the socket. False (logged) if the
    /// port cannot be used; Tick then does nothing.
    bool Open(Holds& holds) noexcept;

    /// The lines received since the last tick to the runner, one runner tick, the replies out.
    void Tick() noexcept;

    /// Drops every queued command, releases every hold and closes the socket. Safe to call twice.
    void Close() noexcept;

private:
    std::optional<Runner> runner_;
    Server                server_;
    std::uint64_t         tick_ = 0;
    bool                  open_ = false;
};

}  // namespace lss::control
