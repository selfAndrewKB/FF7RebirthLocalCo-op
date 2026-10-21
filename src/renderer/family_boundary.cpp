#include "renderer/family_boundary.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"
#include "renderer/family_boundary_list.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace lss::renderer {

namespace {

// Facts read from the database (RE-p2-main-menu-crash.md, pass S-CRASH3 §S3.1, §S3.4; this build).
// The entry bytes hold rel32 calls only, which read the same at any load address.
constexpr std::uint8_t kQueueTaskBytes[] = {0x44, 0x88, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44,
                                            0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55};
constexpr platform::FunctionFact kQueueTask{L"TGraphTask SetupPrereqs/QueueTask to the render thread", 0x1420B8090,
                                            kQueueTaskBytes};
constexpr std::uint8_t kGrowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                       0x00, 0x00, 0x00, 0xE8, 0x24, 0x07, 0x1F, 0x01};
constexpr platform::FunctionFact kGrow{L"ME_CmdBuf_GrowAlloc", 0x140EBE7FC, kGrowBytes};
constexpr std::uint8_t kClearStateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                             0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::FunctionFact kClearState{L"FD3D12CommandContext::ClearState", 0x14081FD74, kClearStateBytes};
constexpr std::uint8_t kSubmitBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                         0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
constexpr platform::FunctionFact kSubmit{L"FD3D12CommandContext::RHISubmitCommandsHint", 0x140D009A8, kSubmitBytes};

constexpr std::uintptr_t kContextVtable     = 0x14697BFA8;  // FD3D12CommandContext
constexpr std::uint32_t  kSubmitSlot        = 0xF0;         // its RHISubmitCommandsHint entry
constexpr std::uintptr_t kImmediateList     = 0x1488EB8F0;  // g_RHICmdList_Immediate_Global (the object)
constexpr std::size_t    kImmediateListSize = 0x48;
constexpr std::size_t    kComputeContext    = 0x28;  // list: ComputeContext, logged against Context

constexpr std::uint32_t kAnyThread = 255;         // ENamedThreads::AnyThread: "the caller's thread is not known"
constexpr std::uint64_t kLogPeriod = 30'000;      // ms between the summary lines

using QueueTaskFn  = void (*)(void* task, const void* prerequisites, std::uint32_t currentThread, bool unlock);
using ContextFn    = void (*)(void* context);

/// A render-thread task laid out as the game's render commands (TGraphTask<...>, S-CRASH3 §S3.1):
/// FBaseGraphTask's vtable, thread and prerequisite count, the task storage, TaskConstructed and the
/// Subsequents event (none: fire and forget).
struct BoundaryTask
{
    const void* const* vtable;
    std::uint32_t      thread        = kAnyThread;  // the queue sets the render thread
    std::int32_t       prerequisites = 1;           // the queue drops it to 0 and dispatches
    std::uint64_t      storage       = 0;
    std::uint8_t       constructed   = 0;           // the queue sets it
    std::uint8_t       padding[7]    = {};
    void*              subsequents   = nullptr;
};
static_assert(offsetof(BoundaryTask, thread) == 0x08 && offsetof(BoundaryTask, prerequisites) == 0x0C &&
              offsetof(BoundaryTask, storage) == 0x10 && offsetof(BoundaryTask, constructed) == 0x18 &&
              offsetof(BoundaryTask, subsequents) == 0x20);

// Set once by InstallFamilyBoundary, then only read; published by g_installed's release store.
QueueTaskFn      g_queueTask     = nullptr;
boundary::GrowFn g_grow          = nullptr;
ContextFn        g_clearState    = nullptr;
ContextFn        g_submit        = nullptr;
std::uintptr_t   g_contextVtable = 0;
std::byte*       g_list          = nullptr;
std::atomic<bool> g_installed{false};

std::atomic<bool> g_faulted{false};

// What happened, for the summary line (written on the game, render and RHI threads).
struct Counts
{
    std::atomic<std::uint64_t> queued{0};    // game thread: tasks queued
    std::atomic<std::uint64_t> appended{0};  // render thread: nodes appended to the list
    std::atomic<std::uint64_t> inlined{0};   // render thread: run at once (the list was executing)
    std::atomic<std::uint64_t> reset{0};     // RHI thread: ClearState + submit run
    std::atomic<std::uint64_t> skipped{0};   // RHI thread: the list's context was not the default direct one
    std::atomic<int>           computeIsContext{-1};  // [list+0x28] == [list+0x20] at the first run (-1: not yet)
} g_counts;

std::uint64_t g_logAt = 0;
std::uint64_t g_loggedQueued = 0;
std::uint64_t g_loggedReset  = 0;

// ------------------------------------------------------------------------------------------------
// RHI thread (or the render thread when there is none): the reset itself
// ------------------------------------------------------------------------------------------------

enum class ContextCheck
{
    DefaultDirect,
    Other,
    Faulted,
};

/// Reads the list's context and checks it is the default direct one. The reads follow engine
/// pointers (context, device, its direct contexts), so they run under a guard.
ContextCheck CheckContext(const std::byte* commandList, std::byte*& context) noexcept
{
    __try
    {
        context        = boundary::Load<std::byte*>(commandList, boundary::list::kContext);
        int  unknown   = -1;
        const int same = boundary::Load<std::byte*>(commandList, kComputeContext) == context ? 1 : 0;
        g_counts.computeIsContext.compare_exchange_strong(unknown, same, std::memory_order_relaxed);
        return boundary::IsDefaultDirectContext(context, g_contextVtable) ? ContextCheck::DefaultDirect
                                                                          : ContextCheck::Other;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return ContextCheck::Faulted;
    }
}

void RunBoundary(const std::byte* commandList) noexcept
{
    if (g_faulted.load(std::memory_order_relaxed))
    {
        return;
    }
    std::byte* context = nullptr;
    switch (CheckContext(commandList, context))
    {
        case ContextCheck::Faulted: g_faulted.store(true, std::memory_order_relaxed); return;
        case ContextCheck::Other: g_counts.skipped.fetch_add(1, std::memory_order_relaxed); return;
        case ContextCheck::DefaultDirect: break;
    }
    // RHIEndFrame's pair on the same context: ClearState, then close, submit and reopen the list.
    // The submit runs only after a ClearState that returned.
    if (!platform::CallGuarded(g_clearState, static_cast<void*>(context)))
    {
        g_faulted.store(true, std::memory_order_relaxed);
        return;
    }
    if (!platform::CallGuarded(g_submit, static_cast<void*>(context)))
    {
        g_faulted.store(true, std::memory_order_relaxed);
        return;
    }
    g_counts.reset.fetch_add(1, std::memory_order_relaxed);
}

/// The node's vtable[0]: Execute(node, list, debug context).
void NodeExecute(void* /*node*/, const std::byte* commandList, void* /*debug*/) noexcept
{
    RunBoundary(commandList);
}

/// The node's vtable[1]: a plain node has nothing to destroy (the walk frees the arena).
void NodeNothing(void* /*node*/, const std::byte* /*commandList*/, void* /*debug*/) noexcept {}

const void* const kNodeVtable[] = {reinterpret_cast<const void*>(&NodeExecute),
                                   reinterpret_cast<const void*>(&NodeNothing)};

// ------------------------------------------------------------------------------------------------
// Render thread: the task
// ------------------------------------------------------------------------------------------------

/// Appends the node; the arena grow is the game's allocator, so it runs under a guard.
bool AppendGuarded() noexcept
{
    __try
    {
        boundary::AppendNode(g_list, kNodeVtable, g_grow);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The task's vtable[1]: ExecuteTask(this, NewTasks&, CurrentThread). The task is ours: it is freed
/// here, not given to the game's small-task allocator.
void TaskExecute(BoundaryTask* task, void* /*newTasks*/, std::uint32_t /*currentThread*/) noexcept
{
    if (g_faulted.load(std::memory_order_relaxed))
    {
        // Off after a fault: nothing more is appended.
    }
    else if (boundary::IsExecuting(g_list))
    {
        g_counts.inlined.fetch_add(1, std::memory_order_relaxed);  // the game's EnqueueLambda rule
        RunBoundary(g_list);
    }
    else if (AppendGuarded())
    {
        g_counts.appended.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        g_faulted.store(true, std::memory_order_relaxed);
    }
    delete task;
}

/// The task's vtable[0]: the scalar deleting destructor, (this, flags). The task graph does not
/// call it for a task that frees itself in ExecuteTask; kept correct in case it does.
BoundaryTask* TaskDestroy(BoundaryTask* task, std::uint32_t flags) noexcept
{
    if ((flags & 1) != 0)
    {
        delete task;
    }
    return task;
}

const void* const kTaskVtable[] = {reinterpret_cast<const void*>(&TaskDestroy),
                                   reinterpret_cast<const void*>(&TaskExecute)};

}  // namespace

bool InstallFamilyBoundary(const platform::GameImage& image) noexcept
{
    const auto queue      = platform::FindFunction(image, kQueueTask);
    const auto grow       = platform::FindFunction(image, kGrow);
    const auto clearState = platform::FindFunction(image, kClearState);
    const auto submit     = platform::FindFunction(image, kSubmit);
    const auto vtable     = image.Rebase(kContextVtable, kSubmitSlot + sizeof(void*));
    const auto slot       = image.ReadPointer(kContextVtable + kSubmitSlot);
    const auto list       = image.Rebase(kImmediateList, kImmediateListSize);
    if (!queue || !grow || !clearState || !submit || !vtable || !list || !slot || *slot != *submit)
    {
        platform::Log(L"family boundary: UNAVAILABLE (a check failed); two families share the state cache as before");
        return false;
    }
    if (!platform::PinThisModule())
    {
        platform::Log(L"family boundary: UNAVAILABLE (the DLL could not be pinned)");
        return false;
    }
    g_queueTask     = reinterpret_cast<QueueTaskFn>(*queue);
    g_grow          = reinterpret_cast<boundary::GrowFn>(*grow);
    g_clearState    = reinterpret_cast<ContextFn>(*clearState);
    g_submit        = reinterpret_cast<ContextFn>(*submit);
    g_contextVtable = *vtable;
    g_list          = reinterpret_cast<std::byte*>(*list);
    g_installed.store(true, std::memory_order_release);  // publishes the facts above
    platform::Log(L"family boundary: between two families, the default direct context runs the game's end-of-frame "
                  L"ClearState and submit, in RHI stream order");
    return true;
}

void QueueFamilyBoundary() noexcept
{
    if (!g_installed.load(std::memory_order_acquire) || g_faulted.load(std::memory_order_relaxed))
    {
        return;
    }
    auto* const task = new (std::nothrow) BoundaryTask{kTaskVtable};
    if (task == nullptr)
    {
        return;
    }
    if (!platform::CallGuarded(g_queueTask, static_cast<void*>(task), static_cast<const void*>(nullptr), kAnyThread,
                               true))
    {
        g_faulted.store(true, std::memory_order_relaxed);  // the task is left: it may be queued
        return;
    }
    g_counts.queued.fetch_add(1, std::memory_order_relaxed);
}

void PollFamilyBoundary() noexcept
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        return;
    }
    static bool faultLogged = false;
    if (g_faulted.load(std::memory_order_relaxed) && !faultLogged)
    {
        faultLogged = true;
        platform::Log(L"family boundary: a call faulted; the boundary reset is off for the rest of the session");
    }
    const std::uint64_t now = GetTickCount64();
    if (now < g_logAt)
    {
        return;
    }
    g_logAt                   = now + kLogPeriod;
    const std::uint64_t queued = g_counts.queued.load(std::memory_order_relaxed);
    const std::uint64_t reset  = g_counts.reset.load(std::memory_order_relaxed);
    if (queued == g_loggedQueued && reset == g_loggedReset)
    {
        return;  // nothing new (one family per frame)
    }
    const int compute = g_counts.computeIsContext.load(std::memory_order_relaxed);
    platform::LogDebug(L"family boundary: {} queued, {} reset in stream order (+{} / +{} in the last period); {} appended, "
                  L"{} run inline, {} skipped (not the default direct context); list compute context {}",
                  queued, reset, queued - g_loggedQueued, reset - g_loggedReset,
                  g_counts.appended.load(std::memory_order_relaxed), g_counts.inlined.load(std::memory_order_relaxed),
                  g_counts.skipped.load(std::memory_order_relaxed),
                  compute < 0 ? L"not seen yet" : (compute == 1 ? L"is its context" : L"differs from its context"));
    g_loggedQueued = queued;
    g_loggedReset  = reset;
}

}  // namespace lss::renderer
