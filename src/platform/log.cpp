#include "platform/log.h"

#include "platform/frame_watch.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <atomic>

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

void WriteLine(std::wstring_view line) noexcept
{
    // UE4SS formats with "{}" placeholders, so the text is passed as an argument, never as the
    // format string: a '{' in a message would otherwise be read as a placeholder.
    // Warning level is the one UE4SS writes to its log file in shipping builds.
    // The sink allocates and formats, so it can throw. This function is noexcept, and an escaping
    // exception would end the game over a log line; a lost line is the lesser failure.
    try
    {
        frame_watch::CountLogLine();
        SendLocked(line);
    }
    catch (...)
    {
    }
}

}  // namespace lss::platform
