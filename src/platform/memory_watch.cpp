#include "platform/memory_watch.h"

#include "platform/binned_pools.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#pragma comment(lib, "Psapi.lib")  // GetProcessMemoryInfo, whatever PSAPI_VERSION the SDK defaults to
#pragma comment(lib, "dxgi.lib")

namespace lss::platform::memory_watch {

namespace {

constexpr DWORD     kSampleMs    = 250;
constexpr ULONGLONG kPeriodMs    = 60'000;
constexpr ULONGLONG kMinLineMs   = 1'000;  // an alarm line comes at most once a second
constexpr ULONGLONG kLowRepeatMs = 10'000;
constexpr std::int64_t kPoolStepPercent = 2;   // a 32 KB pool change this large (of its 512 MB) is logged at once
constexpr double       kPoolHighPercent = 90.0;
constexpr double    kLowFreeGb   = 4.0;
constexpr double    kJumpGb      = 0.5;
constexpr double    kBytesPerGb  = 1024.0 * 1024.0 * 1024.0;

std::atomic<bool> g_started{false};

double Gb(unsigned long long bytes) noexcept
{
    return static_cast<double>(bytes) / kBytesPerGb;
}

struct Card
{
    bool   known  = false;
    double used   = 0.0;  // the game's use of the card's own memory
    double budget = 0.0;  // what Windows lets the game use before it moves resources out
};

/// The adapter with the most dedicated memory (the one the game renders on), or null.
Microsoft::WRL::ComPtr<IDXGIAdapter3> MainAdapter() noexcept
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter3> best;
    SIZE_T                                bestMemory = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;; ++i)
    {
        const HRESULT found = factory->EnumAdapters1(i, &adapter);
        if (found == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        DXGI_ADAPTER_DESC1                    desc{};
        Microsoft::WRL::ComPtr<IDXGIAdapter3> three;
        if (SUCCEEDED(found) && adapter && SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            desc.DedicatedVideoMemory > bestMemory && SUCCEEDED(adapter.As(&three)))
        {
            best       = three;
            bestMemory = desc.DedicatedVideoMemory;
        }
    }
    return best;
}

Card ReadCard(IDXGIAdapter3* adapter) noexcept
{
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (adapter == nullptr || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    {
        return {};
    }
    return {true, Gb(info.CurrentUsage), Gb(info.Budget)};
}

double Percent(const binned_pools::PoolFill& fill) noexcept
{
    return fill.capacity != 0 ? 100.0 * fill.committed / fill.capacity : 0.0;
}

/// The three fullest pools other than the 32 KB one, e.g. "8 KB 12% (983/8192), ...".
std::wstring FullestPools(const binned_pools::Pools& pools)
{
    struct Entry
    {
        std::uint32_t          index = 0;
        binned_pools::PoolFill fill;
    };
    std::array<Entry, 3> top{};
    for (std::uint32_t i = 0; i < binned_pools::kPoolCount; ++i)
    {
        const auto fill = i != binned_pools::kPool32K ? pools.Read(i) : std::nullopt;
        if (!fill)
        {
            continue;
        }
        for (std::size_t slot = 0; slot < top.size(); ++slot)
        {
            if (Percent(*fill) > Percent(top[slot].fill))
            {
                std::move_backward(top.begin() + static_cast<std::ptrdiff_t>(slot), top.end() - 1, top.end());
                top[slot] = {i, *fill};
                break;
            }
        }
    }
    std::wstring text;
    for (const Entry& entry : top)
    {
        if (entry.fill.capacity != 0)
        {
            text += std::format(L"{}{} B {:.1f}% ({}/{})", text.empty() ? L"" : L", ", entry.fill.blockSize,
                                Percent(entry.fill), entry.fill.committed, entry.fill.capacity);
        }
    }
    return text.empty() ? L"none above 0%" : text;
}

DWORD WINAPI Run(void*) noexcept
{
    const auto adapter = MainAdapter();
    const auto image   = GameImage::Locate();
    const auto pools   = image ? binned_pools::Pools::Locate(*image) : std::nullopt;
    Log(L"memory: watching every {} ms on its own thread (graphics card memory {}; the allocator's 32 KB pool {})",
        kSampleMs, adapter ? L"included" : L"UNAVAILABLE",
        pools ? L"included" : L"UNAVAILABLE: not the Binned3 layout the research read");
    std::uint32_t loggedPool = 0;
    bool          loggedPoolKnown = false;
    ULONGLONG lastLog      = 0;
    ULONGLONG lastLogPeriodic = 0;
    double    loggedCommit = 0.0;
    double    loggedCard   = 0.0;
    bool      loggedCardKnown = false;  // a card reading that failed is no baseline for a jump
    bool      loggedOver   = false;
    for (;; Sleep(kSampleMs))
    {
        MEMORYSTATUSEX system{};
        system.dwLength = sizeof(system);
        PROCESS_MEMORY_COUNTERS_EX process{};
        if (!GlobalMemoryStatusEx(&system) ||
            !GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),
                                  sizeof(process)))
        {
            Log(L"memory: UNAVAILABLE (error {}); the watch stops", GetLastError());
            return 0;
        }
        const Card      card       = ReadCard(adapter.Get());
        const ULONGLONG now        = GetTickCount64();
        const double    freeCommit = Gb(system.ullAvailPageFile);
        const double    commit     = Gb(process.PrivateUsage);
        const bool      first      = lastLog == 0;
        const double    change     = first ? 0.0 : commit - loggedCommit;
        const double    cardChange = first || !card.known || !loggedCardKnown ? 0.0 : card.used - loggedCard;
        const bool      low        = freeCommit < kLowFreeGb;
        const bool      over       = card.known && card.used > card.budget;
        const auto      pool       = pools ? pools->Read(binned_pools::kPool32K) : std::nullopt;
        const auto      poolChange = first || !pool || !loggedPoolKnown
                                         ? 0
                                         : static_cast<std::int64_t>(pool->committed) - static_cast<std::int64_t>(loggedPool);
        const bool      poolStep   = pool && pool->capacity != 0 &&
                              (poolChange >= 0 ? poolChange : -poolChange) * 100 >=
                                  static_cast<std::int64_t>(pool->capacity) * kPoolStepPercent;
        const bool      poolHigh   = pool && Percent(*pool) >= kPoolHighPercent;
        const bool      jump       = change >= kJumpGb || change <= -kJumpGb || cardChange >= kJumpGb ||
                            cardChange <= -kJumpGb || poolStep;
        // A lasting state (over budget, low commit, a nearly full pool) is not repeated every second: a change of
        // budget state is, and the others every 10 s.
        const bool alarm = jump || over != loggedOver || ((low || poolHigh) && now - lastLog >= kLowRepeatMs);
        if (!first && !(alarm && now - lastLog >= kMinLineMs) && now - lastLog < kPeriodMs)
        {
            continue;
        }
        const double seconds = first ? 0.0 : static_cast<double>(now - lastLog) / 1000.0;
        lastLog              = now;
        loggedCommit         = commit;
        loggedCard           = card.used;
        loggedCardKnown      = card.known;
        loggedOver           = over;
        loggedPool           = pool ? pool->committed : 0;
        loggedPoolKnown      = pool.has_value();
        // The first line and the alarms are Info; the once-a-minute line with nothing to report is a periodic status
        // line (Debug). The baselines above move either way.
        const Level level = first || alarm ? Level::Info : Level::Debug;
        if (!Enabled(level))
        {
            lastLogPeriodic = now;  // this was the periodic line
            continue;
        }
        try
        {
            const std::wstring poolText =
                pool ? std::format(L"32 KB pool {:.1f}% ({} of {} blocks-of-blocks, {:+}; {} blocks live){}", Percent(*pool),
                                   pool->committed, pool->capacity, poolChange, pool->liveBlocks,
                                   pool->full ? L" FULL: the next refill ends the game" : L"")
                     : std::wstring(L"32 KB pool unread");
            // The periodic line also names the fullest other pools; alarm lines stay short.
            const bool periodic = first || now - lastLogPeriodic >= kPeriodMs;
            if (periodic)
            {
                lastLogPeriodic = now;
            }
            LogAt(level,
                  L"memory: game commit {:.2f} GB ({:+.2f} in {:.1f} s, peak {:.2f}), working set {:.2f} GB; card {:.2f} of "
                  L"{:.2f} GB budget ({:+.2f}); system commit {:.1f} of {:.1f} GB used, {:.1f} GB free; {}{}{}{}{}{}",
                  commit, change, seconds, Gb(process.PeakPagefileUsage), Gb(process.WorkingSetSize), card.used,
                  card.budget, cardChange, Gb(system.ullTotalPageFile - system.ullAvailPageFile),
                  Gb(system.ullTotalPageFile), freeCommit, poolText,
                  periodic && pools ? L"; fullest others: " + FullestPools(*pools) : std::wstring(),
                  jump ? L" (JUMP)" : L"", over ? L" (CARD OVER BUDGET)" : L"",
                  low ? L" (LOW: the engine ends the game when commit runs out)" : L"",
                  poolHigh ? L" (POOL HIGH: the engine ends the game when a pool fills)" : L"");
        }
        catch (...)
        {
        }
    }
}

}  // namespace

void Start() noexcept
{
    if (g_started.exchange(true))
    {
        return;
    }
    // The thread runs this DLL's code until the process exits, so the DLL must stay loaded even on the
    // paths that install nothing else (a failed engine check, the watch-only control).
    if (!PinThisModule())
    {
        Log(L"memory: UNAVAILABLE (the DLL could not be pinned for the watch thread)");
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        Log(L"memory: UNAVAILABLE (the watch thread could not start, error {})", GetLastError());
        return;
    }
    CloseHandle(thread);  // never joined; it ends with the process
}

}  // namespace lss::platform::memory_watch
