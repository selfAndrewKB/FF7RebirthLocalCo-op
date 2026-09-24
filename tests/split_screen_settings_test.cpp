// Host-side tests for players::SplitScreenSettings, run without the game.
//
// The console is a fake holding one integer variable. The cases are the join/leave transitions
// the setting must follow: set once on join, put back on leave, nothing touched with one player,
// a value someone else chose left alone, and failures reported rather than assumed.

#include "players/split_screen_settings.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

using lss::players::ConsoleAccess;
using lss::players::SettingsChange;
using lss::players::SplitScreenSettings;
using Kind = SettingsChange::Kind;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

// The fake console: one variable, and switches to make reading or executing fail.
struct FakeConsole
{
    std::int32_t value         = 0;
    bool         readWorks     = true;
    bool         executeWorks  = true;
    bool         executeIgnored = false;  // the command "runs" but changes nothing
    int          commands      = 0;
} g_console;

std::optional<std::int32_t> ReadInt(const wchar_t* variable) noexcept
{
    if (!g_console.readWorks || std::wcscmp(variable, SplitScreenSettings::kResizeMethod) != 0)
    {
        return std::nullopt;
    }
    return g_console.value;
}

bool Execute(const wchar_t* command) noexcept
{
    ++g_console.commands;
    if (!g_console.executeWorks)
    {
        return false;
    }
    const std::wstring prefix = std::wstring(SplitScreenSettings::kResizeMethod) + L" ";
    const std::wstring text   = command;
    if (!g_console.executeIgnored && text.rfind(prefix, 0) == 0)
    {
        g_console.value = std::stoi(text.substr(prefix.size()));
    }
    return true;
}

SplitScreenSettings Fresh(std::int32_t initial)
{
    g_console = FakeConsole{};
    g_console.value = initial;
    return SplitScreenSettings(ConsoleAccess{&ReadInt, &Execute});
}

void TestJoinSetsAndLeaveRestores()
{
    std::puts("join sets 2 once; leave puts the previous value back");
    auto settings = Fresh(0);
    EXPECT(settings.Update(false).kind == Kind::None);  // one player: nothing touched
    EXPECT(g_console.commands == 0);

    const auto joined = settings.Update(true);
    EXPECT(joined.kind == Kind::Applied && joined.value == 0);
    EXPECT(g_console.value == 2);
    EXPECT(settings.Update(true).kind == Kind::None);  // only on a change
    EXPECT(g_console.commands == 1);

    const auto left = settings.Update(false);
    EXPECT(left.kind == Kind::Restored && left.value == 0);
    EXPECT(g_console.value == 0);
}

void TestAlreadySetIsNotRestored()
{
    std::puts("a value that was already 2 is neither set nor restored");
    auto settings = Fresh(2);
    EXPECT(settings.Update(true).kind == Kind::AlreadySet);
    EXPECT(settings.Update(false).kind == Kind::None);
    EXPECT(g_console.value == 2);
    EXPECT(g_console.commands == 0);
}

void TestSomeoneElsesChoiceIsKept()
{
    std::puts("a value changed while player 2 was in is left alone on leave");
    auto settings = Fresh(0);
    settings.Update(true);
    g_console.value = 1;  // e.g. typed into the console
    const auto left = settings.Update(false);
    EXPECT(left.kind == Kind::LeftAlone && left.value == 1);
    EXPECT(g_console.value == 1);
}

void TestFailuresAreReported()
{
    std::puts("unreadable, unexecutable or ignored commands are reported, not assumed");
    {
        auto settings = Fresh(0);
        g_console.readWorks = false;
        EXPECT(settings.Update(true).kind == Kind::ApplyFailed);
        g_console.readWorks = true;
        EXPECT(settings.Update(false).kind == Kind::None);  // nothing of ours to undo
        EXPECT(g_console.value == 0);
    }
    {
        auto settings = Fresh(0);
        g_console.executeWorks = false;
        EXPECT(settings.Update(true).kind == Kind::ApplyFailed);
    }
    {
        auto settings = Fresh(0);
        g_console.executeIgnored = true;  // read-back shows the command did not take
        EXPECT(settings.Update(true).kind == Kind::ApplyFailed);
        EXPECT(settings.Update(false).kind == Kind::None);
    }
    {
        auto settings = Fresh(0);
        settings.Update(true);
        g_console.executeWorks = false;
        const auto left = settings.Update(false);
        EXPECT(left.kind == Kind::RestoreFailed && left.value == 0);
    }
}

void TestFailedRestoreIsRetriedAtTheNextLeave()
{
    std::puts("a failed restore keeps the old value and retries at the next leave");
    auto settings = Fresh(0);
    settings.Update(true);
    g_console.executeWorks = false;
    EXPECT(settings.Update(false).kind == Kind::RestoreFailed);
    EXPECT(g_console.value == 2);

    g_console.executeWorks = true;
    EXPECT(settings.Update(true).kind == Kind::AlreadySet);  // still 2 from before
    const auto left = settings.Update(false);
    EXPECT(left.kind == Kind::Restored && left.value == 0);  // the original, not 2
    EXPECT(g_console.value == 0);
}

// A console where exactly one read, counted from 0 after Fresh, fails.
int g_reads    = 0;
int g_failRead = -1;

std::optional<std::int32_t> FlakyReadInt(const wchar_t* variable) noexcept
{
    return g_reads++ == g_failRead ? std::nullopt : ReadInt(variable);
}

SplitScreenSettings FreshFlaky(std::int32_t initial, int failRead)
{
    g_console       = FakeConsole{};
    g_console.value = initial;
    g_reads         = 0;
    g_failRead      = failRead;
    return SplitScreenSettings(ConsoleAccess{&FlakyReadInt, &Execute});
}

void TestUnconfirmedApplyIsStillRestored()
{
    std::puts("a command that ran but could not be confirmed is still undone on leave");
    auto settings = FreshFlaky(0, 1);  // read 0: the current value; read 1: the confirmation fails
    EXPECT(settings.Update(true).kind == Kind::ApplyFailed);
    EXPECT(g_console.value == 2);      // the command did take
    const auto left = settings.Update(false);
    EXPECT(left.kind == Kind::Restored && left.value == 0);
    EXPECT(g_console.value == 0);
}

void TestUnreadableValueIsNotOverwrittenOnLeave()
{
    std::puts("an unreadable value on leave is not overwritten; the restore is retried");
    auto settings = FreshFlaky(0, 2);  // reads 0-1: the join; read 2: the leave's check fails
    EXPECT(settings.Update(true).kind == Kind::Applied);
    g_console.value = 1;               // someone else's choice, which the mod cannot see
    EXPECT(settings.Update(false).kind == Kind::RestoreFailed);
    EXPECT(g_console.value == 1);      // not overwritten blind
    EXPECT(g_console.commands == 1);
}

void TestUnreadableLeaveIsRetried()
{
    std::puts("a restore blocked by an unreadable value completes at the next leave");
    auto settings = FreshFlaky(0, 2);
    settings.Update(true);                                   // 0 -> 2
    EXPECT(settings.Update(false).kind == Kind::RestoreFailed);
    EXPECT(g_console.value == 2);
    EXPECT(settings.Update(true).kind == Kind::AlreadySet);  // owed value kept
    const auto left = settings.Update(false);
    EXPECT(left.kind == Kind::Restored && left.value == 0);
    EXPECT(g_console.value == 0);
}

void TestCommandThatDidNotTakeNeedsNoRestore()
{
    std::puts("a command that ran but changed nothing leaves nothing to undo");
    auto settings = Fresh(0);
    g_console.executeIgnored = true;
    EXPECT(settings.Update(true).kind == Kind::ApplyFailed);
    g_console.executeIgnored = false;
    EXPECT(settings.Update(false).kind == Kind::None);
    EXPECT(g_console.value == 0);
    EXPECT(g_console.commands == 1);
}
void TestRejoinAppliesAgain()
{
    std::puts("leave then rejoin applies again");
    auto settings = Fresh(0);
    settings.Update(true);
    settings.Update(false);
    EXPECT(settings.Update(true).kind == Kind::Applied);
    EXPECT(g_console.value == 2);
}

}  // namespace

int main()
{
    TestJoinSetsAndLeaveRestores();
    TestAlreadySetIsNotRestored();
    TestSomeoneElsesChoiceIsKept();
    TestFailuresAreReported();
    TestFailedRestoreIsRetriedAtTheNextLeave();
    TestUnconfirmedApplyIsStillRestored();
    TestUnreadableValueIsNotOverwrittenOnLeave();
    TestUnreadableLeaveIsRetried();
    TestCommandThatDidNotTakeNeedsNoRestore();
    TestRejoinAppliesAgain();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
