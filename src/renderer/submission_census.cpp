#include "renderer/submission_census.h"

#include "facts/engine.h"
#include "platform/build_value.h"
#include "platform/code_memory.h"
#include "platform/command_registry.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/instruction_patch.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "platform/vtable_entry.h"
#include "platform/x64_jump.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace lss::renderer {

namespace {

// Every address below is each build's own IDA address (RE-perf-submission-census.md, both builds re-read from the
// exes on disk). Return addresses cannot go through the translation tables, so the census uses PerBuild throughout
// and turns them into live addresses with the image base.
using Addr                       = platform::PerBuild<std::uintptr_t>;
constexpr std::uintptr_t kIdaBase = 0x140000000;

constexpr Addr kSubmitSlot{0x1468018F8, 0x14697C560};  // FD3D12CommandListManager vtable [1] (.rdata)
constexpr Addr kSubmitFn{0x140AE0798, 0x140D00E88};    // what it holds
constexpr Addr kSignalSlot{0x1468013C8, 0x14697C0B0};  // context vtable +0x108: signal a GPU fence, no flush
constexpr Addr kSignalFn{0x140ADFC1C, 0x140A4B804};
constexpr Addr kHintVtable{facts::kSubmitCommandsHintVTable, 0x146602920};  // FRHICommandSubmitCommandsHint
constexpr Addr kHintExecute{facts::kSubmitCommandsHintExecute, 0x140D00988};
constexpr Addr kBatchingMode{0x146EECFAC, 0x1470B9E34};       // D3D12.CommandListBatchingMode (int)
constexpr Addr kNiagaraHint{0x1490E8C48, 0x1492AF9C4};        // fx.NiagaraGpuSubmitCommandHint (int)
constexpr platform::PerBuild<std::uint32_t> kPendingNum{0x88, 0xD8};  // device: PendingCommandLists.Num

// L1: the return address in the function that called the submit.
constexpr Addr kL1FlushSingle{0x140AE32DE, 0x140D00CA9};  // FlushCommands
constexpr Addr kL1FlushMerged{0x140AE338F, 0x140D00D5A};  // FlushCommands, with the device's pending lists
constexpr Addr kL1HintSingle{0x140AE351A, 0x140D00A9F};   // RHISubmitCommandsHint
constexpr Addr kL1HintMerged{0x140AE35C7, 0x140D00B5A};
constexpr Addr kL1Batch{0x140AE0A02, 0x14090D336};        // SubmitAndFreeContextContainer
// A single list on the second copy manager (device +0x170), closed then submitted with wait: the return address inside,
// found live (called up to once per frame). Not identified in 1.004 (0: logged as unknown there).
constexpr Addr kL1CopyWait{0x140ADFCBD, 0};

// L2 under RHISubmitCommandsHint.
// Every hint node (Execute tail-jumps), from either of the walker's node calls: the whole walker's range (1.005 also
// calls nodes through a cached `call r9`; found live).
constexpr Addr kWalkerBegin{0x140873400, 0x140812430};
constexpr Addr kWalkerEnd{0x140874BCF, 0x1408127C1};
constexpr Addr kL2BeginTransitions{0x14252A0F6, 0x1428EE3FA};
constexpr Addr kL2EndTransitions{0x14087108E, 0x1428EE531};
constexpr Addr kL2FenceFlush{0x141674373, 0x14162919F};
// L2 under FlushCommands.
constexpr Addr kL2Present{0x140ADDF5A, 0x140DF5DC2};
constexpr Addr kL2Dlss{0x141344C75, 0x14137E695};
constexpr Addr kL2BeginFrame{0x140A495A0, 0x140D01008};
constexpr Addr kL2EndFrameGap{0x140A497A9, 0x140D01211};
constexpr Addr kL2EndFrameFlush{0x140A49843, 0x140D012A7};
constexpr Addr kL2QueryResult{0x1407887ED, 0x140840051};
constexpr Addr kL2FlushWaitAll[] = {{0x142237274, 0x14248C60C}, {0x142237298, 0x14248C630}};
constexpr Addr kL2Profiler[]     = {{0x1426BABD8, 0x142A4D240}, {0x1426BADF2, 0x142A4D45A}};
constexpr std::uintptr_t kL2Upload1004[] = {0x140DF536E, 0x14248A548, 0x1429D75D2, 0x142AC6E79, 0x14493422C,
                                            0x144935457, 0x144935842, 0x144935BDC, 0x144937ABC, 0x144937FAA,
                                            0x1449384F8, 0x144938CAE, 0x144939A9B};
constexpr std::uintptr_t kL2Upload[] = {0x140ADEA03, 0x140AE1C55, 0x14262C2AC, 0x142730BE6, 0x1446B76EF,
                                            0x1446C985A, 0x1446CA8A4, 0x1446CAC91, 0x1446CB02F, 0x1446CCF98,
                                            0x1446CD474, 0x1446CD9C8, 0x1446CE163, 0x1446CEF5D};

// Layouts (same in both builds).
constexpr std::uintptr_t kManagerQueueType = 0xF4;    // 0 default, 1 copy, 2 async, 3 second copy
constexpr std::uintptr_t kListContext      = 0x20;    // FD3D12CommandListData: the owning context
constexpr std::uintptr_t kContextDevice    = 0x148;
constexpr std::uintptr_t kContextList      = 0x1D8;   // the context's current list (0 after a deferred finish)
constexpr std::uintptr_t kContextWork      = 0xA2C8;  // six int counters, zeroed when a list opens

enum Bucket : int
{
    HintParallel,       // untagged walker hint on the direct queue: the parallel passes' site (renderer/submit_hints)
    HintRdg,            // tag 1
    HintNiagara,        // tag 2
    HintAsync,          // tag 3
    HintMe,             // tag 4
    HintUntaggedAsync,  // untagged walker hint on another queue (ME's async end where it cannot be tagged)
    BeginTransitions,
    EndTransitions,
    FenceFlush,
    ModBoundary,
    HintOtherCaller,
    Present,
    Dlss,
    BeginFrame,
    EndFrameGap,
    EndFrameFlush,
    QueryResult,
    FlushWaitAll,
    Profiler,
    UploadReadback,
    FlushOtherCaller,
    ParallelBatch,
    CopyQueueWait,
    UnknownPath,
    kBucketCount
};

constexpr const wchar_t* kBucketNames[kBucketCount] = {
    L"hint: parallel passes",    L"hint: SE RDG passes",   L"hint: Niagara",          L"hint: async compute",
    L"hint: ME async end",       L"hint: untagged, async", L"begin transitions",      L"end transitions",
    L"GPU fence write (flush)",  L"the mod's boundary",    L"hint path, other caller", L"Present",
    L"DLSS",                     L"begin frame",           L"end frame: gap recorder", L"end frame: default flush",
    L"occlusion/query read",     L"flush all + wait",      L"GPU profiler",           L"upload/readback",
    L"flush, other caller",      L"parallel batch",        L"copy queue 2, with wait", L"unknown path"};

struct BucketStats
{
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> lists{0};
    std::atomic<std::uint64_t> work[6]{};   // over lists whose counters are current
    std::atomic<std::uint64_t> stale{0};    // lists whose context had moved on (counters not theirs)
    std::atomic<std::uint64_t> waits{0};    // bWaitForCompletion
    std::atomic<std::uint64_t> ticks{0};    // QPC inside the engine's submit
    std::atomic<std::uint64_t> offThread{0};  // not on the thread that submits Present (the RHI thread)
};

std::array<BucketStats, kBucketCount>      g_buckets;
std::array<std::atomic<std::uint64_t>, 4>  g_queues{};
std::atomic<std::uint64_t>                 g_frames{0};
std::atomic<std::uint64_t>                 g_stackMismatch{0};
std::array<std::atomic<std::uint64_t>, 5>  g_hintNoSubmit{};   // per tag: the hint ran but did not submit
std::array<std::atomic<std::uint64_t>, 8>  g_unknown{};        // (L1 rva << 32) | L2 rva, last few
std::atomic<std::uint32_t>                 g_unknownNext{0};
std::atomic<std::uint64_t>                 g_signals{0};
std::atomic<std::uint64_t>                 g_signalsWithWork{0};
std::atomic<std::uint64_t>                 g_signalsWithPending{0};
std::atomic<std::uint32_t>                 g_presentThread{0};

struct Live
{
    std::uintptr_t base = 0;
    std::uintptr_t l1[6]{};  // FlushSingle, FlushMerged, HintSingle, HintMerged, Batch, CopyWait
    std::uintptr_t walkerBegin = 0, walkerEnd = 0, beginTransitions = 0, endTransitions = 0, fenceFlush = 0;
    std::uintptr_t present = 0, dlss = 0, beginFrame = 0, endFrameGap = 0, endFrameFlush = 0, queryResult = 0;
    std::uintptr_t flushWaitAll[2]{}, profiler[2]{};
    std::uintptr_t upload[14]{};
    std::size_t    uploadCount = 0;
    std::uintptr_t moduleBegin = 0, moduleEnd = 0;  // this DLL (the mod's boundary submits from it)
    std::uint32_t  pendingNum  = 0;
};

Live                  g_live;
platform::VTableEntry g_submitEntry;
platform::VTableEntry g_signalEntry;
std::uintptr_t        g_execute = 0;
bool                  g_installed = false;
thread_local int      t_hintTag   = 0;  // set by a tagged hint thunk; -1 once the submit took it

std::uintptr_t LiveOf(std::uintptr_t ida) noexcept
{
    return g_live.base + (ida - kIdaBase);
}

std::uint32_t RvaOf(std::uintptr_t live) noexcept
{
    return live >= g_live.base && live - g_live.base < 0x20000000 ? static_cast<std::uint32_t>(live - g_live.base) : 0;
}

void RememberUnknown(std::uintptr_t l1, std::uintptr_t l2) noexcept
{
    const auto slot = g_unknownNext.fetch_add(1, std::memory_order_relaxed) % g_unknown.size();
    g_unknown[slot].store((static_cast<std::uint64_t>(RvaOf(l1)) << 32) | RvaOf(l2), std::memory_order_relaxed);
}

Bucket Classify(std::uintptr_t l1, std::uintptr_t l2, int tag, std::uint32_t queueType) noexcept
{
    const Live& v = g_live;
    if (l1 == v.l1[2] || l1 == v.l1[3])  // RHISubmitCommandsHint
    {
        if (tag >= 1 && tag <= 4)
        {
            return static_cast<Bucket>(HintRdg + tag - 1);
        }
        if (l2 >= v.walkerBegin && l2 < v.walkerEnd) return queueType == 0 ? HintParallel : HintUntaggedAsync;
        if (l2 == v.beginTransitions) return BeginTransitions;
        if (l2 == v.endTransitions) return EndTransitions;
        if (l2 == v.fenceFlush) return FenceFlush;
        if (l2 >= v.moduleBegin && l2 < v.moduleEnd) return ModBoundary;
        RememberUnknown(l1, l2);
        return HintOtherCaller;
    }
    if (l1 == v.l1[0] || l1 == v.l1[1])  // FlushCommands
    {
        if (l2 == v.present) return Present;
        if (l2 == v.dlss) return Dlss;
        if (l2 == v.beginFrame) return BeginFrame;
        if (l2 == v.endFrameGap) return EndFrameGap;
        if (l2 == v.endFrameFlush) return EndFrameFlush;
        if (l2 == v.queryResult) return QueryResult;
        if (l2 == v.flushWaitAll[0] || l2 == v.flushWaitAll[1]) return FlushWaitAll;
        if (l2 == v.profiler[0] || l2 == v.profiler[1]) return Profiler;
        for (std::size_t i = 0; i < v.uploadCount; ++i)
        {
            if (l2 == v.upload[i]) return UploadReadback;
        }
        RememberUnknown(l1, l2);
        return FlushOtherCaller;
    }
    if (l1 == v.l1[4]) return ParallelBatch;
    if (v.l1[5] != 0 && l1 == v.l1[5]) return CopyQueueWait;
    RememberUnknown(l1, l2);
    return UnknownPath;
}

/// The lists' recorded work, read before the submit (the flush paths reopen, and so zero, the counters after it).
void CountLists(BucketStats& stats, const void* listArray) noexcept
{
    struct
    {
        const std::byte* data;
        std::int32_t     num;
    } array{};
    if (!platform::CopyGuarded(&array, listArray, sizeof(array)) || array.num <= 0)
    {
        return;
    }
    stats.lists.fetch_add(static_cast<std::uint64_t>(array.num), std::memory_order_relaxed);
    for (std::int32_t i = 0; i < array.num && i < 16; ++i)
    {
        const std::byte* listData = nullptr;
        const std::byte* context  = nullptr;
        const std::byte* current  = nullptr;
        std::int32_t     work[6]  = {};
        if (!platform::CopyGuarded(&listData, array.data + 16 * i + 8, sizeof(listData)) || listData == nullptr ||
            !platform::CopyGuarded(&context, listData + kListContext, sizeof(context)) || context == nullptr ||
            !platform::CopyGuarded(&current, context + kContextList, sizeof(current)) ||
            !platform::CopyGuarded(work, context + kContextWork, sizeof(work)))
        {
            continue;
        }
        if (current != listData && current != nullptr)
        {
            stats.stale.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (int k = 0; k < 6; ++k)
        {
            stats.work[k].fetch_add(static_cast<std::uint64_t>(work[k] > 0 ? work[k] : 0), std::memory_order_relaxed);
        }
    }
}

using SubmitFn = std::uint64_t (*)(void*, void*, std::uint64_t, void*);

/// The managers' vtable [1]: Submit(manager, TArray<list handle>*, bWaitForCompletion). Any thread.
__declspec(noinline) std::uint64_t CensusSubmit(void* manager, void* lists, std::uint64_t wait, void* r9) noexcept
{
    void*                frames[2] = {};
    const auto           got       = RtlCaptureStackBackTrace(1, 2, frames, nullptr);
    const std::uintptr_t l1        = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uintptr_t l2        = got >= 2 ? reinterpret_cast<std::uintptr_t>(frames[1]) : 0;
    if (got < 1 || reinterpret_cast<std::uintptr_t>(frames[0]) != l1)
    {
        g_stackMismatch.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint32_t queueType = 0;
    platform::CopyGuarded(&queueType, static_cast<const std::byte*>(manager) + kManagerQueueType, sizeof(queueType));
    const int tag = t_hintTag > 0 ? t_hintTag : 0;
    if (tag > 0)
    {
        t_hintTag = -1;
    }
    const Bucket bucket = Classify(l1, l2, tag, queueType);
    BucketStats& stats  = g_buckets[bucket];
    stats.calls.fetch_add(1, std::memory_order_relaxed);
    if ((wait & 0xFF) != 0)
    {
        stats.waits.fetch_add(1, std::memory_order_relaxed);
    }
    g_queues[queueType & 3].fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t thread = GetCurrentThreadId();
    if (bucket == Present)
    {
        g_presentThread.store(thread, std::memory_order_relaxed);
    }
    else if (thread != g_presentThread.load(std::memory_order_relaxed))
    {
        stats.offThread.fetch_add(1, std::memory_order_relaxed);
    }
    CountLists(stats, lists);

    LARGE_INTEGER before{};
    LARGE_INTEGER after{};
    QueryPerformanceCounter(&before);
    const auto result = reinterpret_cast<SubmitFn>(g_submitEntry.Original())(manager, lists, wait, r9);
    QueryPerformanceCounter(&after);
    stats.ticks.fetch_add(static_cast<std::uint64_t>(after.QuadPart - before.QuadPart), std::memory_order_relaxed);
    if (bucket == Present)
    {
        g_frames.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

using SignalFn = void (*)(void*, void*, void*, void*);

/// The contexts' vtable +0x108: a fence signal on the queue with no flush. Counts the ones issued while recorded work
/// is not submitted yet (the open list has work, or the device holds parked lists).
void CensusSignal(void* context, void* fence, void* r8, void* r9) noexcept
{
    g_signals.fetch_add(1, std::memory_order_relaxed);
    const auto*      ctx     = static_cast<const std::byte*>(context);
    std::int32_t     work[6] = {};
    const std::byte* device  = nullptr;
    std::int32_t     pending = 0;
    if (platform::CopyGuarded(work, ctx + kContextWork, sizeof(work)) &&
        (work[0] | work[1] | work[2] | work[3] | work[4] | work[5]) != 0)
    {
        g_signalsWithWork.fetch_add(1, std::memory_order_relaxed);
    }
    if (platform::CopyGuarded(&device, ctx + kContextDevice, sizeof(device)) && device != nullptr &&
        platform::CopyGuarded(&pending, device + g_live.pendingNum, sizeof(pending)) && pending > 0)
    {
        g_signalsWithPending.fetch_add(1, std::memory_order_relaxed);
    }
    reinterpret_cast<SignalFn>(g_signalEntry.Original())(context, fence, r8, r9);
}

// ------------------------------------------------------------------------------------------------
// Hint tags
// ------------------------------------------------------------------------------------------------

using ExecuteFn = void (*)(void*, void*, void*);

template <int Tag>
void HintThunk(void* node, void* commandList, void* debug) noexcept
{
    t_hintTag = Tag;
    reinterpret_cast<ExecuteFn>(g_execute)(node, commandList, debug);
    if (t_hintTag == Tag)  // the context had nothing to submit (an async context may skip)
    {
        g_hintNoSubmit[Tag].fetch_add(1, std::memory_order_relaxed);
    }
    t_hintTag = 0;
}

struct HintSite
{
    const wchar_t*             name;
    Addr                       site;
    const void*                thunk;
    platform::InstructionPatch patch;  // the lea's displacement, pointed at the tagged vtable copy
};

std::array<HintSite, 4> g_hints = {{
    {L"SE RDG passes", {0x1427CE170, 0x14251391E}, reinterpret_cast<const void*>(&HintThunk<1>)},
    {L"Niagara", {0x140C8269E, 0x140CF7344}, reinterpret_cast<const void*>(&HintThunk<2>)},
    {L"async compute", {0x1446E75A7, 0x14494ED29}, reinterpret_cast<const void*>(&HintThunk<3>)},
    {L"ME async end", {0x1447DE481, 0x1424D183B}, reinterpret_cast<const void*>(&HintThunk<4>)},
}};

/// Points one producer's `lea rax,[hint vtable]` at a copy whose slot 0 is its thunk. The rel32 must lie in one
/// aligned 16-byte block (one atomic swap); where it does not, the site stays untagged.
const wchar_t* TagHint(HintSite& hint, std::uintptr_t vtable, std::uintptr_t stub) noexcept
{
    if (hint.patch.WasApplied())  // tagged before: the copy exists, the same displacement goes in again
    {
        return hint.patch.Reapply() ? L"tagged" : L"its code changed";
    }
    const std::uintptr_t site     = LiveOf(hint.site);
    std::uint8_t         lea[7]   = {};
    std::int32_t         original = 0;
    if (!platform::CopyGuarded(lea, reinterpret_cast<const void*>(site), sizeof(lea)) || lea[0] != 0x48 ||
        lea[1] != 0x8D || lea[2] != 0x05)
    {
        return L"not the lea the research read";
    }
    std::memcpy(&original, lea + 3, sizeof(original));
    if (site + 7 + original != vtable)
    {
        return L"its lea does not load the hint vtable";
    }
    const std::uintptr_t rel   = site + 3;
    const std::uintptr_t block = rel & ~(platform::kCodeBlockSize - 1);
    if (((rel + 3) & ~(platform::kCodeBlockSize - 1)) != block)
    {
        return L"untagged (its displacement crosses a 16-byte block)";
    }
    const void* const    slots[2] = {hint.thunk, reinterpret_cast<const void*>(stub)};
    const std::uintptr_t copy     = platform::PlaceCodeNear(site, sizeof(slots), [&](std::uintptr_t, std::span<std::uint8_t> code) noexcept {
        std::memcpy(code.data(), slots, sizeof(slots));
        return true;
    });
    const auto displacement = copy != 0 ? platform::Rel32(site + 7, copy) : std::nullopt;
    if (!displacement)
    {
        return L"no room for the vtable copy within reach";
    }
    std::uint8_t tagged[sizeof(original)] = {};
    std::memcpy(tagged, &*displacement, sizeof(tagged));
    return hint.patch.Apply(rel, std::span<const std::uint8_t>(lea + 3, sizeof(original)), tagged) ? L"tagged"
                                                                                               : L"its code changed";
}

void UntagHints() noexcept
{
    for (HintSite& hint : g_hints)
    {
        if (hint.patch.IsApplied() && !hint.patch.Restore())
        {
            platform::Log(L"submission census: the {} hint site changed after us; left as it is", hint.name);
        }
    }
}

bool ThisModule(std::uintptr_t& begin, std::uintptr_t& end) noexcept
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&CensusSubmit), &module))
    {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::byte*>(module) + dos->e_lfanew);
    begin           = reinterpret_cast<std::uintptr_t>(module);
    end             = begin + nt->OptionalHeader.SizeOfImage;
    return true;
}

void FillLive(std::uintptr_t base) noexcept
{
    Live& v = g_live;
    v.base  = base;
    const Addr l1[6] = {kL1FlushSingle, kL1FlushMerged, kL1HintSingle, kL1HintMerged, kL1Batch, kL1CopyWait};
    for (int i = 0; i < 6; ++i)
    {
        const std::uintptr_t ida = l1[i];
        v.l1[i] = ida != 0 ? LiveOf(ida) : 0;
    }
    v.walkerBegin      = LiveOf(kWalkerBegin);
    v.walkerEnd        = LiveOf(kWalkerEnd);
    v.beginTransitions = LiveOf(kL2BeginTransitions);
    v.endTransitions   = LiveOf(kL2EndTransitions);
    v.fenceFlush       = LiveOf(kL2FenceFlush);
    v.present          = LiveOf(kL2Present);
    v.dlss             = LiveOf(kL2Dlss);
    v.beginFrame       = LiveOf(kL2BeginFrame);
    v.endFrameGap      = LiveOf(kL2EndFrameGap);
    v.endFrameFlush    = LiveOf(kL2EndFrameFlush);
    v.queryResult      = LiveOf(kL2QueryResult);
    for (int i = 0; i < 2; ++i)
    {
        v.flushWaitAll[i] = LiveOf(kL2FlushWaitAll[i]);
        v.profiler[i]     = LiveOf(kL2Profiler[i]);
    }
    const bool v1004 = platform::RunningBuild() == platform::BuildIndex::V1004;
    v.uploadCount    = v1004 ? std::size(kL2Upload1004) : std::size(kL2Upload);
    for (std::size_t i = 0; i < v.uploadCount; ++i)
    {
        v.upload[i] = LiveOf(v1004 ? kL2Upload1004[i] : kL2Upload[i]);
    }
    v.pendingNum = kPendingNum;
}

std::int32_t ReadInt(Addr address) noexcept
{
    std::int32_t value = -999;  // unreadable: a number no count takes
    platform::Read(LiveOf(address), value);
    return value;
}

void ResetWindow() noexcept
{
    for (BucketStats& b : g_buckets)
    {
        b.calls.store(0, std::memory_order_relaxed);
        b.lists.store(0, std::memory_order_relaxed);
        for (auto& w : b.work)
        {
            w.store(0, std::memory_order_relaxed);
        }
        b.stale.store(0, std::memory_order_relaxed);
        b.waits.store(0, std::memory_order_relaxed);
        b.ticks.store(0, std::memory_order_relaxed);
        b.offThread.store(0, std::memory_order_relaxed);
    }
    for (auto& q : g_queues) q.store(0, std::memory_order_relaxed);
    for (auto& h : g_hintNoSubmit) h.store(0, std::memory_order_relaxed);
    g_frames.store(0, std::memory_order_relaxed);
    g_stackMismatch.store(0, std::memory_order_relaxed);
    g_signals.store(0, std::memory_order_relaxed);
    g_signalsWithWork.store(0, std::memory_order_relaxed);
    g_signalsWithPending.store(0, std::memory_order_relaxed);
}

}  // namespace

bool InstallSubmissionCensus() noexcept
{
    if (g_installed)
    {
        return true;
    }
    const auto image = platform::GameImage::Locate();
    if (!image || image->Build() == nullptr)
    {
        platform::Log(L"submission census: not installed (an unknown game build)");
        return false;
    }
    FillLive(image->Base());
    if (!ThisModule(g_live.moduleBegin, g_live.moduleEnd))
    {
        platform::Log(L"submission census: not installed (this DLL's range is unknown)");
        return false;
    }
    g_execute = LiveOf(kHintExecute);
    ResetWindow();
    if (!g_submitEntry.Replace(LiveOf(kSubmitSlot), LiveOf(kSubmitFn), reinterpret_cast<std::uintptr_t>(&CensusSubmit)))
    {
        platform::Log(L"submission census: not installed (the command-list manager's submit slot holds something else)");
        return false;
    }
    g_installed = true;
    const bool signals =
        g_signalEntry.Replace(LiveOf(kSignalSlot), LiveOf(kSignalFn), reinterpret_cast<std::uintptr_t>(&CensusSignal));
    // The thunks call Execute directly and the copies keep slot 1: both must be what the live vtable holds.
    std::uintptr_t slots[2]  = {};
    std::uint8_t   stubCode[3] = {};
    const bool     vtableOk  = platform::CopyGuarded(slots, reinterpret_cast<const void*>(LiveOf(kHintVtable)), sizeof(slots)) &&
                          slots[0] == g_execute && slots[1] != 0 &&
                          platform::CopyGuarded(stubCode, reinterpret_cast<const void*>(slots[1]), sizeof(stubCode)) &&
                          stubCode[0] == 0xC2 && stubCode[1] == 0x00 && stubCode[2] == 0x00;
    std::wstring tags;
    for (HintSite& hint : g_hints)
    {
        tags += std::format(L"{}{} {}", tags.empty() ? L"" : L"; ", hint.name,
                            vtableOk ? TagHint(hint, LiveOf(kHintVtable), slots[1]) : L"untagged (the hint vtable differs)");
    }
    platform::Log(L"submission census: on. Submit slot wrapped; fence signals without flush {}; hint producers: {}. "
                  L"Batching mode {}, fx.NiagaraGpuSubmitCommandHint {}",
                  signals ? L"counted" : L"not counted (slot differs)", tags, ReadInt(kBatchingMode), ReadInt(kNiagaraHint));
    return true;
}

void InstallSubmissionCensusIfAsked() noexcept
{
    if (platform::FlagFile(L"census.txt"))
    {
        InstallSubmissionCensus();
    }
}

void RemoveSubmissionCensus() noexcept
{
    if (!g_installed)
    {
        return;
    }
    UntagHints();
    g_signalEntry.Restore();
    g_submitEntry.Restore();
    g_installed = false;
    platform::Log(L"submission census: off (hooks restored)");
}

bool SubmissionCensusInstalled() noexcept
{
    return g_installed;
}

void RegisterSubmissionCensusCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"census",
                       .shape = control::Shape::OnOff,
                       .help  = L"diagnosis: the GPU submission census in the perf log's window (off at start; "
                                L"census.txt: on)",
                       .set   = [](const Value& v) {
                           if (!std::get<bool>(v))
                           {
                               RemoveSubmissionCensus();
                               return Outcome{true, L"submission census off"};
                           }
                           if (!InstallSubmissionCensus())
                           {
                               return Outcome{false, L"the submission census could not install (see the log)"};
                           }
                           return Outcome{true, L"submission census on (summary in the perf log)"};
                       },
                       .get = [] { return Value{SubmissionCensusInstalled()}; }});
}

void LogSubmissionCensus() noexcept
{
    if (!g_installed)
    {
        return;
    }
    static const double ticksPerMicrosecond = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1e6;
    }();
    const std::uint64_t frames = g_frames.load(std::memory_order_relaxed);
    if (frames == 0)
    {
        platform::Log(L"submission census: no Present seen in this window");
        ResetWindow();
        return;
    }
    const double  perFrame = 1.0 / static_cast<double>(frames);
    std::uint64_t total    = 0;
    for (const BucketStats& b : g_buckets)
    {
        total += b.calls.load(std::memory_order_relaxed);
    }
    platform::Log(L"submission census over {} frames: {:.2f} submissions per frame (queues: default {:.2f}, copy {:.2f}, "
                  L"async {:.2f}, copy 2 {:.2f}); fence signals without flush {:.2f}/f, {:.2f}/f with open work, {:.2f}/f "
                  L"with parked lists; stack mismatches {}",
                  frames, total * perFrame, g_queues[0].load() * perFrame, g_queues[1].load() * perFrame,
                  g_queues[2].load() * perFrame, g_queues[3].load() * perFrame, g_signals.load() * perFrame,
                  g_signalsWithWork.load() * perFrame, g_signalsWithPending.load() * perFrame, g_stackMismatch.load());
    for (int i = 0; i < kBucketCount; ++i)
    {
        const BucketStats& b     = g_buckets[i];
        const std::uint64_t calls = b.calls.load(std::memory_order_relaxed);
        if (calls == 0)
        {
            continue;
        }
        const double perCall = 1.0 / static_cast<double>(calls);
        platform::Log(L"  {}: {:.2f}/f, {:.1f} lists each, per submission draws {:.0f} dispatches {:.0f} clears {:.0f} "
                      L"barriers {:.0f} copies {:.0f} other {:.0f}; stale lists {}, waits {}, off the RHI thread {}, {:.0f} us/f in "
                      L"the submit",
                      kBucketNames[i], calls * perFrame, b.lists.load() * perCall, b.work[0].load() * perCall,
                      b.work[1].load() * perCall, b.work[2].load() * perCall, b.work[3].load() * perCall,
                      b.work[4].load() * perCall, b.work[5].load() * perCall, b.stale.load(), b.waits.load(), b.offThread.load(),
                      b.ticks.load() * perFrame / ticksPerMicrosecond);
    }
    const std::uint64_t noSubmit = g_hintNoSubmit[1] + g_hintNoSubmit[2] + g_hintNoSubmit[3] + g_hintNoSubmit[4];
    if (noSubmit != 0)
    {
        platform::Log(L"  tagged hints that did not submit: RDG {}, Niagara {}, async {}, ME {}", g_hintNoSubmit[1].load(),
                      g_hintNoSubmit[2].load(), g_hintNoSubmit[3].load(), g_hintNoSubmit[4].load());
    }
    std::wstring unknown;
    for (const auto& u : g_unknown)
    {
        const std::uint64_t value = u.load(std::memory_order_relaxed);
        if (value != 0)
        {
            unknown += std::format(L" {:#x}/{:#x}", kIdaBase + (value >> 32), kIdaBase + (value & 0xFFFFFFFF));
        }
    }
    if (!unknown.empty())
    {
        platform::Log(L"  unknown callers (this build's IDA L1/L2):{}", unknown);
    }
    ResetWindow();
}

}  // namespace lss::renderer
