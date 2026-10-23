// Host-side tests for control::Runner, run without the game, against a registry and holds of its own.
// Taken from Remake's tests/control_runner_test.cpp; added: a query waits its turn behind its client's queue, Busy per
// client, the structured replies.

#include "control/runner.h"

#include "expect.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace lss::control;

/// The fake mod: a join that can be made to fail, a switch, a counter that scripts wait on.
struct Fake final : Holds
{
    bool                      joinFails = false;
    bool                      holdFails = false;
    bool                      bars      = true;
    std::int64_t              counter   = 0;
    Value                     mode      = Value{0.5};  // a reading that is a number or a named value
    std::vector<std::wstring> calls;
    std::vector<std::int32_t> held;  // players whose stick is held now

    Outcome HoldStick(std::int32_t player, const float (&)[4]) override
    {
        calls.push_back(L"hold " + std::to_wstring(player));
        if (holdFails)
        {
            return {false, L"per-pad sticks are off"};
        }
        held.push_back(player);
        return {};
    }
    void ReleaseStick(std::int32_t player) override
    {
        calls.push_back(L"release " + std::to_wstring(player));
        std::erase(held, player);
    }
    bool Held(std::int32_t player) const override
    {
        return std::find(held.begin(), held.end(), player) != held.end();
    }
    /// The mod ends a hold on its own (a leave): no release call reaches it from the runner.
    void EndOnItsOwn(std::int32_t player) { std::erase(held, player); }

    Registry MakeRegistry()
    {
        Registry registry;
        registry.Add({.name = L"join", .help = L"join", .set = [this](const Value&) {
                          calls.push_back(L"join");
                          return joinFails ? Outcome{false, L"no"} : Outcome{};
                      }});
        registry.Add({.name = L"leave", .help = L"leave", .set = [this](const Value&) {
                          calls.push_back(L"leave");
                          return Outcome{true, L"removed"};
                      }});
        registry.Add({.name = L"bars", .shape = Shape::OnOff, .help = L"bars",
                      .set  = [this](const Value& v) { bars = std::get<bool>(v); return Outcome{}; },
                      .get  = [this] { return Value{bars}; }});
        registry.Add({.name = L"counter", .shape = Shape::Integer, .max = 1000, .help = L"counter",
                      .get  = [this] { return Value{counter}; }});
        registry.Add({.name = L"mode", .shape = Shape::Float, .minFloat = 0.0, .maxFloat = 1.0, .words = {L"native"},
                      .help = L"mode", .get = [this] { return mode; }});
        return registry;
    }
};

struct Harness
{
    Fake          fake;
    Registry      registry = fake.MakeRegistry();
    Runner        runner{registry, fake};
    std::uint64_t tick = 0;

    std::vector<std::wstring> Replies(ClientId client = 0)
    {
        std::vector<std::wstring> out;
        for (const auto& reply : runner.TakeReplies())
        {
            if (client == 0 || reply.client == client)
            {
                out.push_back(ConsoleText(reply));
            }
        }
        return out;
    }
    void Ticks(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            runner.Tick(++tick);
        }
    }
};

void TestActionsAndReplies()
{
    std::puts("runner: actions run on the next tick, in order, each replying ok with its detail");
    Harness h;
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"leave");
    EXPECT(h.Replies().empty());  // nothing runs before a tick
    EXPECT(h.runner.Busy(1) && !h.runner.Busy(2));
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 join" && replies[1] == L"ok 2 leave -- removed");
    EXPECT((h.fake.calls == std::vector<std::wstring>{L"join", L"leave"}));
    EXPECT(h.runner.Idle() && !h.runner.Busy(1));
}

void TestStructuredReplies()
{
    std::puts("runner: a reply carries its kind, line, command and detail apart");
    Harness h;
    h.runner.Submit(7, L"leave");
    h.Ticks(1);
    const auto replies = h.runner.TakeReplies();
    EXPECT(replies.size() == 1);
    EXPECT(replies.size() == 1 && replies[0].client == 7 && replies[0].kind == ReplyKind::Ok && replies[0].line == 1 &&
           replies[0].command == L"leave" && replies[0].detail == L"removed");
}

void TestRejectedAndQueries()
{
    std::puts("runner: a bad line is rejected at once; a query on an empty queue answers at once");
    Harness h;
    h.runner.Submit(1, L"nonsense");
    h.runner.Submit(1, L"get counter");
    h.runner.Submit(1, L"note marker");
    auto replies = h.Replies();
    EXPECT(replies.size() == 4);  // rejected; value + ok; note
    EXPECT(replies.size() == 4 && replies[0].starts_with(L"rejected 1 nonsense -- "));
    EXPECT(replies.size() == 4 && replies[1] == L"value counter 0" && replies[2] == L"ok 2 get counter");
    EXPECT(replies.size() == 4 && replies[3] == L"note 3 marker");
    h.runner.Submit(1, L"status");
    replies = h.Replies();
    EXPECT(replies.size() == 4 && replies[0] == L"value bars on" && replies[1] == L"value counter 0" &&
           replies[2] == L"value mode 0.5" && replies[3] == L"ok 4 status");
    h.runner.Submit(1, L"help counter");
    replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"help counter <0..1000> -- (read only) counter");
    h.runner.Submit(1, L"help");
    replies = h.Replies();
    EXPECT(replies.size() == 6 && replies[0] == L"help join -- (action) join" &&
           replies[2] == L"help bars on|off -- bars" && replies[4] == L"help mode <0..1>|native -- (read only) mode");
}

void TestQueriesWaitTheirTurn()
{
    std::puts("runner: a script's query behind a wait answers after the wait, in its place");
    Harness h;
    h.runner.Submit(1, L"wait-until counter == 5 100");
    h.runner.Submit(1, L"note after the wait");
    h.runner.Submit(1, L"get counter");
    h.runner.Submit(1, L"join");
    EXPECT(h.Replies().empty());  // queued behind the wait, not answered at submit
    h.Ticks(2);
    EXPECT(h.Replies().empty());
    h.fake.counter = 5;
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT(replies.size() == 5);
    EXPECT(replies.size() == 5 && replies[0] == L"ok 1 wait-until counter == 5 100" &&
           replies[1] == L"note 2 after the wait" && replies[2] == L"value counter 5" &&
           replies[3] == L"ok 3 get counter" && replies[4] == L"ok 4 join");

    std::puts("runner: another client's query still answers during a wait");
    h.runner.Submit(1, L"wait 3");
    h.runner.Submit(2, L"get counter");
    EXPECT(h.Replies(2).size() == 2);
}

void TestWaitBlocks()
{
    std::puts("runner: a wait of N ticks ends N ticks after the tick it started on");
    Harness h;
    h.runner.Submit(1, L"wait 3");
    h.runner.Submit(1, L"join");
    h.Ticks(3);
    EXPECT(h.Replies().empty());
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 wait 3" && replies[1] == L"ok 2 join");
}

void TestWaitUntil()
{
    std::puts("runner: wait-until ends when the reading compares true, at once if it already does, else times out");
    Harness h;
    h.runner.Submit(1, L"wait-until counter >= 2 5");
    h.runner.Submit(1, L"join");
    h.Ticks(2);
    EXPECT(h.Replies().empty());
    h.fake.counter = 2;
    h.Ticks(1);
    auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 wait-until counter >= 2 5" && replies[1] == L"ok 2 join");

    h.runner.Submit(1, L"wait-until bars == on");
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 1 && replies[0] == L"ok 3 wait-until bars == on 600");

    h.runner.Submit(1, L"wait-until counter == 99 3");
    h.runner.Submit(1, L"leave");
    h.Ticks(3);
    EXPECT(h.Replies().empty());
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"failed 4 wait-until counter == 99 3 -- timed out after 3 ticks" &&
           replies[1] == L"dropped 5 leave -- after a failure");
    EXPECT(h.runner.Idle());
}

void TestHoldsRunAlongsideAndRelease()
{
    std::puts("runner: a hold runs alongside the commands after it; the client stays busy until it ends");
    Harness h;
    h.runner.Submit(1, L"hold 2 stick 0 1 0 0 2");
    h.runner.Submit(1, L"join");
    h.Ticks(1);
    auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"ok 1 hold 2 stick 0 1 0 0 2" && replies[1] == L"ok 2 join");
    EXPECT((h.fake.held == std::vector<std::int32_t>{2}));
    EXPECT(!h.runner.Idle() && h.runner.Busy(1));
    h.Ticks(2);
    EXPECT(h.fake.held.empty());
    EXPECT(h.runner.Idle() && !h.runner.Busy(1));

    std::puts("runner: a failure releases the client's holds and drops what it queued");
    h.fake.joinFails = true;
    h.runner.Submit(1, L"hold 1 stick 1 0 0 0 100");
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"leave");
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 3 && replies[1] == L"failed 4 join -- no" &&
           replies[2] == L"dropped 5 leave -- after a failure");
    EXPECT(h.fake.held.empty());
}

void TestClientsAreSeparate()
{
    std::puts("runner: one client's failure and holds leave another's alone");
    Harness h;
    h.fake.joinFails = true;
    h.runner.Submit(1, L"hold 1 stick 1 0 0 0 100");
    h.runner.Submit(2, L"hold 2 stick 1 0 0 0 100");
    h.runner.Submit(1, L"join");   // fails
    h.runner.Submit(2, L"leave");  // another client's command still runs
    h.runner.Submit(1, L"leave");  // dropped
    h.Ticks(1);
    const auto one = h.Replies(1);
    EXPECT(one.size() == 3 && one[1] == L"failed 2 join -- no" && one[2] == L"dropped 3 leave -- after a failure");
    EXPECT((h.fake.held == std::vector<std::int32_t>{2}));
    EXPECT(!h.runner.Busy(1) && h.runner.Busy(2));
    h.runner.ClientClosed(2);
    EXPECT(h.fake.held.empty());
}

void TestClientsQueuesAreIndependent()
{
    std::puts("runner: a client's wait holds only its own queue; a client that leaves drops its queue");
    Harness h;
    h.runner.Submit(1, L"wait 100");
    h.runner.Submit(1, L"leave");
    h.runner.Submit(2, L"join");
    h.Ticks(1);
    const auto two = h.Replies(2);
    EXPECT(two.size() == 1 && two[0] == L"ok 1 join");
    EXPECT(h.Replies(1).empty());
    h.runner.ClientClosed(1);
    const auto one = h.Replies(1);
    EXPECT(one.size() == 2 && one[0] == L"dropped 1 wait 100 -- the client went away" &&
           one[1] == L"dropped 2 leave -- the client went away");
    EXPECT(h.runner.Idle());
}

void TestAbortAndBudget()
{
    std::puts("runner: at most kMaxCommandsPerTick a tick; abort drops every queue at once");
    Harness h;
    for (int i = 0; i < 40; ++i)
    {
        h.runner.Submit(1, L"join");
    }
    h.Ticks(1);
    EXPECT(h.Replies().size() == Runner::kMaxCommandsPerTick);
    h.runner.Submit(2, L"wait 50");
    h.runner.Submit(1, L"abort");  // client 1's 8 left and client 2's wait
    const auto replies = h.Replies();
    EXPECT(replies.size() == 10 && replies.back() == L"ok 41 abort");
    EXPECT(h.runner.Idle());
}

void TestLineNumbersAreFileLines()
{
    std::puts("runner: blank lines and comments count, so a script's numbers are its file lines");
    Harness h;
    h.runner.Submit(1, L"# a comment");
    h.runner.Submit(1, L"");
    h.runner.Submit(1, L"join");
    h.runner.Submit(1, L"nonsense");
    const auto first = h.Replies();
    EXPECT(first.size() == 1 && first[0].starts_with(L"rejected 4 nonsense -- "));
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT(replies.size() == 1 && replies[0] == L"ok 3 join");
}

void TestCrossKindWait()
{
    std::puts("runner: a number never equals a named value; != holds between them");
    Harness h;
    h.runner.Submit(1, L"wait-until mode != native 5");
    h.Ticks(1);
    auto replies = h.Replies();
    EXPECT(replies.size() == 1 && replies[0] == L"ok 1 wait-until mode != native 5");
    h.runner.Submit(1, L"wait-until mode == native 2");
    h.Ticks(1);
    EXPECT(h.Replies().empty());
    h.fake.mode = Value{std::wstring(L"native")};
    h.Ticks(1);
    replies = h.Replies();
    EXPECT(replies.size() == 1 && replies[0] == L"ok 2 wait-until mode == native 2");
}

void TestFailedHoldAndQueuedQuery()
{
    std::puts("runner: a refused hold fails the script, adds no hold and releases the client's others");
    Harness h;
    h.runner.Submit(1, L"hold 1 stick 1 0 0 0 100");
    h.Ticks(1);
    h.Replies();
    h.fake.holdFails = true;
    h.runner.Submit(1, L"hold 2 stick 0 1 0 0 100");
    h.runner.Submit(1, L"get counter");  // queued behind the hold: dropped with it
    h.Ticks(1);
    const auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"failed 2 hold 2 stick 0 1 0 0 100 -- per-pad sticks are off" &&
           replies[1] == L"dropped 3 get counter -- after a failure");
    EXPECT(h.fake.held.empty());  // player 1's earlier hold released with the failure
    EXPECT(!h.runner.Busy(1));
}

void TestHoldEndedByTheMod()
{
    std::puts("runner: a hold the mod ended on its own no longer keeps its client busy");
    Harness h;
    h.runner.Submit(1, L"hold 2 stick 0 1 0 0 36000");
    h.Ticks(1);
    h.Replies();
    EXPECT(h.runner.Busy(1));
    h.fake.EndOnItsOwn(2);  // a leave: the session's hold layer released it
    h.Ticks(1);
    EXPECT(!h.runner.Busy(1) && h.runner.Idle());
    EXPECT(h.fake.calls.back() == L"hold 2");  // no second release from the runner
}

void TestShutdown()
{
    std::puts("runner: shutting down drops every queue and releases every hold");
    Harness h;
    h.runner.Submit(1, L"hold 1 stick 1 0 0 0 100");
    h.runner.Submit(1, L"wait 50");
    h.runner.Submit(2, L"join");
    h.Ticks(1);
    h.Replies();
    h.runner.Submit(2, L"wait 10");
    h.runner.Shutdown();
    const auto replies = h.Replies();
    EXPECT(replies.size() == 2 && replies[0] == L"dropped 2 wait 50 -- the console closed" &&
           replies[1] == L"dropped 2 wait 10 -- the console closed");
    EXPECT(h.fake.held.empty() && h.runner.Idle());
}

}  // namespace

int main()
{
    TestActionsAndReplies();
    TestStructuredReplies();
    TestRejectedAndQueries();
    TestQueriesWaitTheirTurn();
    TestWaitBlocks();
    TestWaitUntil();
    TestHoldsRunAlongsideAndRelease();
    TestClientsAreSeparate();
    TestClientsQueuesAreIndependent();
    TestAbortAndBudget();
    TestLineNumbersAreFileLines();
    TestCrossKindWait();
    TestFailedHoldAndQueuedQuery();
    TestHoldEndedByTheMod();
    TestShutdown();

    return lss::test::Report();
}
