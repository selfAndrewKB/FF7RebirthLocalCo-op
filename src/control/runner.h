#pragma once

// Runs the control commands against the registry. Game thread only; pure logic with no engine access, tested on the
// host (tests/control_runner_test.cpp).
//
// Each client has its own queue, so a client is its own script: its actions and switches run in order, a bounded
// number per tick; its `wait` and `wait-until` hold only its own queue back; its `hold`s run alongside the commands
// after them until their ticks run out. A query (get, status, help, note) answers at once when its client has
// nothing queued, and otherwise takes its place in the queue, so a script's `status` after a wait reports after the
// wait and its notes mark the log where they stand. A failure drops the commands the client queued behind it and
// releases its holds: a script stops at its first failure. A client that goes away drops its queue and releases its
// holds, so a later player on the same pad never inherits one. `abort` does that for every client. Two clients
// acting on the game at once interleave by tick.
//
// Every reply is structured (Reply), so each front end writes it its own way: the file inbox in its result-file
// lines (control/command_channel.h), the console in one line per reply (ConsoleText):
//   ok <n> <command> [-- <detail>]      the client's n-th line completed (a wait: when it ended)
//   failed <n> <command> -- <reason>
//   rejected <n> <line> -- <reason>     the line did not parse; nothing ran
//   dropped <n> <command> -- <reason>   queued, never run (after a failure, an abort, or the client left)
//   value <name> <value>                from get and status
//   help <name> <shape> -- <text>       from help
//   note <n> <text>                     the note, echoed
//
// Taken from Remake's control/runner (2026-10-23). The query's turn in the queue was found missing in Remake while
// porting (a script's status after a wait reported before the wait); Remake took the same fix in its v0.70.1 the same
// day. Changed: replies are structured; Busy(client) tells a front end when one client's script is through; every
// submitted line is numbered, blanks and comments included, so a script's numbers are its file lines; a hold the mod
// ended on its own (Holds::Held) leaves the runner's count.

#include "control/registry.h"
#include "control/script.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace lss::control {

/// The input holds the mod can make, injected at its own per-pad layer so a scripted hold takes a real pad's path.
class Holds
{
public:
    virtual ~Holds() = default;

    virtual Outcome HoldStick(std::int32_t player, const float (&axes)[4]) = 0;
    virtual void    ReleaseStick(std::int32_t player)                      = 0;
    /// The player's stick is still held: false once the mod released it on its own (the join it was made in ended).
    virtual bool    Held(std::int32_t player) const                        = 0;
};

using ClientId = std::uint32_t;

enum class ReplyKind : std::uint8_t
{
    Ok,
    Failed,
    Rejected,
    Dropped,
    Value,
    Help,
    Note,
};

struct Reply
{
    ClientId      client = 0;
    ReplyKind     kind   = ReplyKind::Ok;
    std::uint32_t line   = 0;  // the client's line number, every line counted (0 for value and help lines)
    std::wstring  command;     // the command as written (Describe); rejected: the line; value, help: the name
    std::wstring  detail;      // ok: what it did (may be empty); failed, rejected, dropped: why; value: the value;
                               // help: the shape and the help; note: the text
};

/// The reply as the console writes it (the list above).
std::wstring ConsoleText(const Reply& reply);

class Runner
{
public:
    /// A queue without waits still spreads over ticks, so it never stalls one frame for long.
    static constexpr std::size_t kMaxCommandsPerTick = 32;

    Runner(const Registry& registry, Holds& holds) noexcept : registry_(registry), holds_(holds) {}

    /// One line from a client: parsed now (a bad line is rejected at once), a query answered now or queued behind
    /// the client's commands, an action queued, an abort acted on. Replies go to TakeReplies.
    void Submit(ClientId client, std::wstring_view line);

    /// The client went away: its queue is dropped and its holds released.
    void ClientClosed(ClientId client);

    /// Advances every client's queue by one tick: holds count down, waits end, queued commands run.
    void Tick(std::uint64_t tick);

    /// Releases every hold and drops every queue (the front end is closing).
    void Shutdown();

    std::vector<Reply> TakeReplies();

    /// No client has anything queued or held.
    bool Idle() const noexcept;

    /// The client has something queued or a hold still running.
    bool Busy(ClientId client) const noexcept;

private:
    struct Queued
    {
        std::uint32_t line = 0;
        Command       command;
    };
    struct Client
    {
        ClientId           id        = 0;
        std::uint32_t      lines     = 0;  // the last line number given
        std::deque<Queued> queue;
        bool               waiting   = false;  // the head is a wait that has begun
        std::uint32_t      waitTicks = 0;
    };
    struct Hold
    {
        ClientId      client = 0;
        std::int32_t  player = 0;
        std::uint32_t ticks  = 0;
    };

    Client& Find(ClientId client);
    void    Send(ClientId client, ReplyKind kind, std::uint32_t line, std::wstring command, std::wstring detail = {});
    void    Answer(const Client& client, std::uint32_t line, const Command& command);
    void    Advance(Client& client);
    bool    Run(Client& client);  // the head command; false if the queue must stop for this tick (a wait began)
    bool    ConditionHolds(const Command& command) const;
    void    Fail(Client& client, const Queued& failed, const std::wstring& reason);
    void    DropQueue(Client& client, std::wstring_view reason);
    void    ReleaseHolds(ClientId client, bool everyClient);

    const Registry&     registry_;
    Holds&              holds_;
    std::vector<Client> clients_;
    std::vector<Hold>   held_;
    std::vector<Reply>  replies_;
};

}  // namespace lss::control
