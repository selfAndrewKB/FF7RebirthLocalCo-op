#include "platform/thread_times.h"

#include "platform/log.h"

#include <cstdint>
#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <intrin.h>

namespace lss::platform::thread_times {

namespace {

struct Tracked
{
    const wchar_t* prefix;  // the engine's thread name, or null for the caller (the game thread)
    HANDLE         handle = nullptr;
    ULONG64        lastCycles = 0;
    std::uint64_t  lastTime   = 0;  // kernel + user, 100 ns units (GetThreadTimes)
};

Tracked       g_threads[] = {{nullptr}, {L"RenderThread"}, {L"RHIThread"}};
LARGE_INTEGER g_qpcLast{};
std::uint64_t g_tscLast = 0;

/// The thread's kernel + user CPU time in 100 ns units, or 0 if it could not be read.
std::uint64_t CpuTime(HANDLE handle) noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(handle, &created, &exited, &kernel, &user))
    {
        return 0;
    }
    const auto value = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return value(kernel) + value(user);
}

bool Alive(HANDLE handle) noexcept
{
    DWORD code = 0;
    return handle != nullptr && GetExitCodeThread(handle, &code) && code == STILL_ACTIVE;
}

/// Opens the first thread of this process whose engine-given name starts with `prefix`.
HANDLE OpenNamed(const wchar_t* prefix) noexcept
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return nullptr;
    }
    HANDLE        found = nullptr;
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD   self = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more && found == nullptr; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != self)
        {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr)
        {
            continue;
        }
        PWSTR name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &name)) && name != nullptr &&
            std::wcsncmp(name, prefix, std::wcslen(prefix)) == 0)
        {
            found = thread;
        }
        LocalFree(name);
        if (found == nullptr)
        {
            CloseHandle(thread);
        }
    }
    CloseHandle(snapshot);
    return found;
}

void Refresh(Tracked& tracked) noexcept
{
    if (Alive(tracked.handle))
    {
        return;
    }
    if (tracked.handle != nullptr)
    {
        CloseHandle(tracked.handle);
    }
    tracked.handle = tracked.prefix == nullptr
                         ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId())
                         : OpenNamed(tracked.prefix);
    tracked.lastCycles = 0;
    tracked.lastTime   = 0;
    if (tracked.handle != nullptr)
    {
        QueryThreadCycleTime(tracked.handle, &tracked.lastCycles);
        tracked.lastTime = CpuTime(tracked.handle);
    }
}

}  // namespace

void EndWindow(std::uint32_t frames, bool log) noexcept
{
    LARGE_INTEGER qpc{}, frequency{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&frequency);
    const std::uint64_t tsc = __rdtsc();

    // Primary: GetThreadTimes deltas (CPU time as Windows accounts it). Cross-check: thread cycles over the TSC rate
    // measured in this window, which Microsoft does not guarantee to be time (QueryThreadCycleTime's documentation);
    // where both agree the line can be trusted, and on a virtual machine they may not.
    double times[3]  = {-1.0, -1.0, -1.0};
    double cycles[3] = {-1.0, -1.0, -1.0};
    const double seconds = g_qpcLast.QuadPart != 0
                               ? static_cast<double>(qpc.QuadPart - g_qpcLast.QuadPart) / static_cast<double>(frequency.QuadPart)
                               : 0.0;
    const double tscHz = seconds > 0.0 ? static_cast<double>(tsc - g_tscLast) / seconds : 0.0;
    for (int i = 0; i < 3; ++i)
    {
        Tracked& tracked = g_threads[i];
        if (!Alive(tracked.handle))
        {
            Refresh(tracked);  // a new or replaced thread starts counting from now
            continue;
        }
        const std::uint64_t time = CpuTime(tracked.handle);
        if (frames != 0 && tracked.lastTime != 0 && time >= tracked.lastTime)
        {
            times[i] = static_cast<double>(time - tracked.lastTime) / 10000.0 / frames;
        }
        tracked.lastTime = time;
        ULONG64 count = 0;
        if (QueryThreadCycleTime(tracked.handle, &count))
        {
            if (tscHz > 0.0 && frames != 0 && tracked.lastCycles != 0)
            {
                cycles[i] = 1000.0 * static_cast<double>(count - tracked.lastCycles) / tscHz / frames;
            }
            tracked.lastCycles = count;
        }
    }
    g_qpcLast = qpc;
    g_tscLast = tsc;
    if (log && times[0] >= 0.0)
    {
        platform::Log(L"  CPU time per frame: game thread {:.2f} ms, render thread {:.2f} ms, RHI thread {:.2f} ms "
                      L"(cycle cross-check {:.2f} / {:.2f} / {:.2f}; negative: thread not found yet)",
                      times[0], times[1], times[2], cycles[0], cycles[1], cycles[2]);
    }
}

}  // namespace lss::platform::thread_times
