#include "platform/log.h"

#include "platform/level_names.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <DynamicOutput/DynamicOutput.hpp>

#include <atomic>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lss::platform {

namespace {

// UE4SS's send takes no lock (its devices' lock() is empty), and the mod writes from the game thread
// and the memory watch's own thread: one line at a time. An SRW lock, released in __finally, because
// this build does not unwind C++ objects for a structured exception (/EHsc): a fault inside the send
// caught further up must not leave the lock held. It has no destructor, so the watch thread may still
// log while the process exits.
SRWLOCK            g_writeLock = SRWLOCK_INIT;
std::atomic<DWORD> g_writer{0};  // the thread inside the send
std::atomic<unsigned> g_lines{0};  // lines written

/// Sends one line under the lock. A line logged from inside the send on the same thread (re-entry) is
/// dropped: the lock is not recursive.
void SendLocked(std::wstring_view line)  // not noexcept: the sink's exceptions pass to WriteLine
{
    const DWORD self = GetCurrentThreadId();
    if (g_writer.load(std::memory_order_relaxed) == self)
    {
        return;
    }
    AcquireSRWLockExclusive(&g_writeLock);
    g_writer.store(self, std::memory_order_relaxed);
    __try
    {
        RC::Output::send<RC::LogLevel::Warning>(STR("[LocalSplitScreen] {}\n"), line);
    }
    __finally
    {
        g_writer.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_writeLock);
    }
}

}  // namespace

namespace detail {
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Info)};
}

void SetLevel(Level level) noexcept
{
    detail::g_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

Level CurrentLevel() noexcept
{
    return static_cast<Level>(detail::g_level.load(std::memory_order_relaxed));
}

const wchar_t* LevelName(Level level) noexcept
{
    return LevelWord(level).data();  // level_names.h's words are literals: null-terminated
}
void WriteLine(std::wstring_view line) noexcept
{
    // UE4SS formats with "{}" placeholders, so the text is passed as an argument, never as the
    // format string: a '{' in a message would otherwise be read as a placeholder.
    // Warning level is the one UE4SS writes to its log file in shipping builds.
    // The sink allocates and formats, so it can throw. This function is noexcept, and an escaping
    // exception would end the game over a log line; a lost line is the lesser failure.
    try
    {
        g_lines.fetch_add(1, std::memory_order_relaxed);
        SendLocked(line);
    }
    catch (...)
    {
    }
}

unsigned LinesWritten() noexcept
{
    return g_lines.load(std::memory_order_relaxed);
}

#if LSS_DEV

void RegisterLogCommands()
{
    control::Register(
        {.name  = L"log-level",
         .shape = control::Shape::Word,
         .words = {L"error", L"warning", L"info", L"debug", L"trace"},
         .help  = L"the most detailed log level written (info at start; debug adds per-event diagnostics, trace "
                  L"per-frame and per-input reports)",
         .set   = [](const control::Value& v) {
             const auto level = ParseLevel(std::get<std::wstring>(v));
             if (!level)
             {
                 return control::Outcome{false, L"no such log level"};
             }
             SetLevel(*level);
             Log(L"log level: {} (lines up to it are written)", LevelName(CurrentLevel()));
             return control::Outcome{true, std::wstring(L"log level ") + LevelName(CurrentLevel())};
         },
         .get = [] { return control::Value{std::wstring(LevelWord(CurrentLevel()))}; }});
}

#endif

}  // namespace lss::platform
