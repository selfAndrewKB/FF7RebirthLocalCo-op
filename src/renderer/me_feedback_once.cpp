#include "renderer/me_feedback_once.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "renderer/me_shadow_feedback_code.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::renderer {

namespace {

// Facts read from the database (RE-p2-distant-lod-swap.md §9.4, §9.9; this build). Each function's first
// bytes are checked before anything is redirected; the entry redirects replace one register save.
constexpr std::uintptr_t kRenderEntry = 0x1457A6720;  // the scene renderer's vtable 0x1457A66F8, slot 5
constexpr std::uint8_t   kRenderMainBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                               0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// SceneRenderer_Render_Main(renderer, RHI command list)
constexpr platform::FunctionFact kRenderMain{L"SceneRenderer_Render_Main", 0x140FDD1F0, kRenderMainBytes};

// The gate's call of ME_Group_ProcessStreamingFeedbackReadbacks(group), its only caller.
constexpr std::uintptr_t kProcessCall = 0x140915E38;
constexpr std::uint8_t   kProcessBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::FunctionFact kProcess{L"ME_Group_ProcessStreamingFeedbackReadbacks", 0x1409156B4, kProcessBytes};

// ME_MainPass_CopyStreamingFeedbackToReadback(builder, scene): its one call (0x14157B30C) crosses a
// 16-byte block, so its entry is redirected; first instruction `mov [rsp+10h],rdx`.
constexpr std::uint8_t   kCopyBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C,
                                         0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54};
constexpr platform::FunctionFact kCopy{L"ME_MainPass_CopyStreamingFeedbackToReadback", 0x140915958, kCopyBytes};
constexpr std::size_t    kCopyFirst   = 5;

// ME_Scene_ComputeRequestedMips_HiddenComponents(renderer, command list, &Views[0]): the same; first
// instruction `mov [rsp+18h],r8`.
constexpr std::uint8_t   kHiddenBytes[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54,
                                           0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54};
constexpr platform::FunctionFact kHidden{L"ME_Scene_ComputeRequestedMips_HiddenComponents", 0x140CF05F8, kHiddenBytes};
constexpr std::size_t    kHiddenFirst   = 5;

// Game version 1.005 (RE-1005-hud-2.md): the process call (0x140BC0ECC there) and both entries (0x14199FE6C,
// 0x14078389C, now `mov [rsp+10h],rdx` and `mov [rsp+10h],rbx`) cross a 16-byte block. Each of the three functions has
// one caller, all on the render thread inside Render_Main, so their calls are redirected instead: the hidden pass's
// (inside one block) at install, the process and copy calls (crossing) from the render thread itself, at the first
// Render (InstallOnRenderThread), and those two are never restored from another thread.
constexpr std::uintptr_t kCopyCall   = 0x14157B30C;  // ME_RenderChunkDispatch_ViewLoop's call of the copy
constexpr std::uintptr_t kHiddenCall = 0x140FDD6EE;  // Render_Main's call of the hidden pass

// Phase 8 of MassiveEnvironment_RenderPass (RE-perf-family-duplication.md §3.1): its sync call of
// ME_DispatchComputeRequestedMeshAndTextureMips(command list, ?, group, chunk, byte stack arguments), once per loaded
// chunk: the slave frustums' mip requests, whose marks (entry+0x48, set from the returned bool) only the main pass's
// copy reads. The call lies inside one 16-byte block in both builds (1.005 0x140C8B5D4 -> 0x140CD1594). The async
// variant's call (only with async compute and r.MassiveEnvironment.UsingAsyncComputeForMainPass) is not covered.
constexpr std::uintptr_t kMipRequestsCall    = 0x140F10D4A;
constexpr std::uint8_t   kMipRequestsBytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                                0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
constexpr platform::FunctionFact kMipRequests{L"ME_DispatchComputeRequestedMeshAndTextureMips", 0x140C9D2F0,
                                              kMipRequestsBytes};

// Option D of RE-perf-me-shadows.md (Phase 3): the shadow pass's streaming-feedback writes, skipped in an additional
// family by machine code at one instruction of ME_ChunkDraw_FullPipeline_cand (renderer/me_shadow_feedback_code.h).
// Research build; 1.005 0x140C882B7 (site), 0x140C882BD, 0x140C883D9 and 0x140C88A01.
constexpr std::uintptr_t kShadowSite   = 0x140F0DC3A;  // jbe over the pre-transition loop (count 0)
constexpr std::uintptr_t kShadowResume = 0x140F0DC40;  // the instruction after it
constexpr std::uintptr_t kShadowEmpty  = 0x140F0DD5C;  // the jbe's target
constexpr std::uintptr_t kShadowSkip   = 0x140F0E345;  // the transition array's free, past the region
constexpr std::uint32_t  kShadowMaskBit = 16;           // feedback-mask

constexpr std::uint32_t kRendererAdditionalFamily = 0x84;  // the family copy's bAdditionalViewFamily (+0x74)

// The budget state, for the log: IStreamingManager's static (FStreamingManagerCollection*), its ME
// streaming manager at +0x50, the render thread's copy of the state at +0xA1 (2 or more: no slave requests).
constexpr std::uintptr_t kStreamingManagers = 0x149117AF8;
constexpr std::uint32_t  kMeManager         = 0x50;
// 1.005: the ME streaming manager's state pair moved 8 down: cmp byte [rax+99h],2 after [collection+50h] at
// 0x140CD1645 (1.004 0x140C9D379 [rcx+0A1h]); its constructor clears +0x98/+0x99 at 0x144FC53C9.
constexpr platform::PerBuild<std::uint32_t> kBudgetStateRender = {0xA1, 0x99};

constexpr ULONGLONG kLogPeriodMs = 10'000;

using RenderFn  = void (*)(void* renderer, void* commandList, void* a3, void* a4);
using ProcessFn = void (*)(void* group);
using CopyFn    = void (*)(void* builder, void* scene);
using HiddenFn  = void (*)(void* renderer, void* commandList, const void* views);
// The stack arguments are bytes (one on the research build, two on 1.005); whole slots are passed on.
using MipRequestsFn = bool (*)(void* commandList, void* a2, void* group, std::uint64_t chunk, std::uint64_t s1,
                               std::uint64_t s2, std::uint64_t s3);

platform::VTableEntry   g_render;
platform::CallRedirect  g_processCall;
platform::EntryRedirect g_copyEntry;
platform::EntryRedirect g_hiddenEntry;
platform::CallRedirect  g_copyCall;    // 1.005, in place of g_copyEntry
platform::CallRedirect  g_hiddenCall;  // 1.005, in place of g_hiddenEntry
platform::CallRedirect  g_mipRequestsCall;
platform::CodeRedirect  g_shadowCode;
ShadowFeedbackFields    g_shadowFields;
std::uint64_t           g_shadowLogged[3] = {};  // skipped, passedOne, passedAdditional at the last log
std::atomic<bool>       g_renderThreadPending{false};  // 1.005: the process and copy calls wait for the render thread
std::uintptr_t          g_processSite = 0, g_processTarget = 0, g_copySite = 0, g_copyTarget = 0;  // set before it
std::atomic<bool>       g_installed{false};
std::atomic<bool>       g_on{false};
std::atomic<std::uint32_t> g_mask{0x1F};  // which skips act (feedback-mask): bit (stage - 1) for each Stage below
std::uintptr_t          g_managers = 0;  // live address of the static

enum Family : int
{
    PlayerOne = 0,          // player 1's family (or any render that is not an additional family's)
    AdditionalPassed = 1,   // an additional family, switch off
    AdditionalSkipped = 2,  // an additional family, the call skipped
    kFamilies = 3,
};

enum Stage : int
{
    Renders = 0,
    Processes = 1,
    Copies = 2,
    Hidden = 3,
    MipRequests = 4,  // counted per chunk
    kStages = 5,
};

// The family the calling thread is rendering, set for the length of one Render_Main: only calls made inside
// it on the same thread see it, whatever other threads render meanwhile.
thread_local Family t_family = PlayerOne;

std::atomic<std::uint64_t> g_counts[kStages][kFamilies];
std::uint64_t              g_logged[kStages][kFamilies] = {};
ULONGLONG                  g_logAt = 0;

Family Current() noexcept
{
    return t_family;
}

/// The shadow code's switch byte: on while player 1's family alone hands in requests, with its mask bit set.
void UpdateShadowSwitch() noexcept
{
    const bool skip = g_on.load(std::memory_order_relaxed) && (g_mask.load(std::memory_order_relaxed) & kShadowMaskBit) != 0;
    std::atomic_ref<std::uint64_t>(g_shadowFields.skip).store(skip ? 1 : 0, std::memory_order_relaxed);
}

std::uint64_t ShadowCount(std::uint64_t& field) noexcept
{
    return std::atomic_ref<std::uint64_t>(field).load(std::memory_order_relaxed);
}

/// Option D, optional: the feature stands without it. The code reads only its fields (this DLL is pinned).
void InstallShadowSkip(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kShadowSite, sizeof(kShadowFeedbackSiteBytes));
    const auto resume = image.Rebase(kShadowResume);
    const auto empty  = image.Rebase(kShadowEmpty);
    const auto skipTo = image.Rebase(kShadowSkip);
    const bool v1005  = platform::RunningBuild() == platform::BuildIndex::V1005;
    const std::span<const std::uint8_t> compare =
        v1005 ? std::span<const std::uint8_t>(kShadowFeedbackCompareBytes1005) : std::span<const std::uint8_t>(kShadowFeedbackCompareBytes);
    std::uint8_t before[4] = {};
    if (!site || !resume || !empty || !skipTo || *resume != *site + sizeof(kShadowFeedbackSiteBytes) ||
        !platform::CopyGuarded(before, reinterpret_cast<const void*>(*site - sizeof(before)), sizeof(before)) ||
        !std::equal(compare.begin(), compare.end(), before))
    {
        platform::Log(L"stream feedback: the shadow pass's streaming writes are not what the research read; players 2 and 3's "
                      L"families still record them");
        return;
    }
    UpdateShadowSwitch();
    const bool installed =
        v1005 ? g_shadowCode.Install(*site, kShadowFeedbackSiteBytes, ShadowFeedbackCode1005(g_shadowFields, *skipTo, *empty), *resume)
              : g_shadowCode.Install(*site, kShadowFeedbackSiteBytes, ShadowFeedbackCode(g_shadowFields, *skipTo, *empty), *resume);
    platform::Log(L"stream feedback: players 2 and 3's families {} the shadow pass's streaming-feedback writes",
                  installed ? L"skip" : L"still record (the site could not be redirected)");
}

/// True if this stage's call is skipped: an additional family under stream-feedback p1, with the stage's mask bit set.
bool Skips(Stage stage, Family family) noexcept
{
    return family == AdditionalSkipped && (g_mask.load(std::memory_order_relaxed) & (1u << (stage - 1))) != 0;
}

void Count(Stage stage, Family family) noexcept
{
    g_counts[stage][family].fetch_add(1, std::memory_order_relaxed);
}

void Process(void* group) noexcept;
void Copy(void* builder, void* scene) noexcept;

/// 1.005: the process and copy calls, from the render thread (the only one that runs them), outside Render_Main.
void InstallOnRenderThread() noexcept
{
    // A plain load first: on the research build (and after the first Render) this is all a Render pays.
    if (!g_renderThreadPending.load(std::memory_order_relaxed) ||
        !g_renderThreadPending.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    const bool process =
        g_processCall.InstallFromOwnThread(g_processSite, g_processTarget, reinterpret_cast<std::uintptr_t>(&Process));
    const bool copy = g_copyCall.InstallFromOwnThread(g_copySite, g_copyTarget, reinterpret_cast<std::uintptr_t>(&Copy));
    platform::Log(L"stream feedback: from the render thread, the readback processing call {} and the main pass's copy "
                  L"call {}",
                  process ? L"installed" : L"FAILED (players 2 and 3's families still process readbacks)",
                  copy ? L"installed" : L"FAILED (players 2 and 3's families still copy their feedback)");
}

/// Replaces the scene renderer's Render (vtable slot): marks an additional family for its length.
void Render(void* renderer, void* commandList, void* a3, void* a4) noexcept
{
    InstallOnRenderThread();  // once, on 1.005
    std::uint8_t additional = 0;
    platform::CopyGuarded(&additional, static_cast<const std::uint8_t*>(renderer) + kRendererAdditionalFamily, 1);
    const Family previous = t_family;  // a render nested inside another on this thread restores the outer one
    t_family              = additional == 0                            ? PlayerOne
                            : g_on.load(std::memory_order_relaxed)     ? AdditionalSkipped
                                                                       : AdditionalPassed;
    Count(Renders, t_family);
    reinterpret_cast<RenderFn>(g_render.Original())(renderer, commandList, a3, a4);
    t_family = previous;
}

/// Replaces the gate's call of the readback processing.
void Process(void* group) noexcept
{
    const Family family = Current();
    Count(Processes, family);
    if (!Skips(Processes, family))
    {
        reinterpret_cast<ProcessFn>(g_processCall.Original())(group);
    }
}

/// Replaces the main pass's feedback copy (entry).
void Copy(void* builder, void* scene) noexcept
{
    const Family family = Current();
    Count(Copies, family);
    if (!Skips(Copies, family))
    {
        // Only one of the two is installed (1.005: the call); each sets its original before it can be reached.
        const std::uintptr_t original = g_copyCall.Original() != 0 ? g_copyCall.Original() : g_copyEntry.Original();
        reinterpret_cast<CopyFn>(original)(builder, scene);
    }
}

/// Replaces the hidden-component pass (entry).
void HiddenPass(void* renderer, void* commandList, const void* views) noexcept
{
    const Family family = Current();
    Count(Hidden, family);
    if (!Skips(Hidden, family))
    {
        const std::uintptr_t original = g_hiddenCall.Original() != 0 ? g_hiddenCall.Original() : g_hiddenEntry.Original();
        reinterpret_cast<HiddenFn>(original)(renderer, commandList, views);
    }
}

/// Replaces phase 8's call of the mip-request dispatches for one chunk. Skipped: nothing is recorded, so false (the
/// chunk is not marked; only the skipped copy would read the mark).
bool MipRequestsCall(void* commandList, void* a2, void* group, std::uint64_t chunk, std::uint64_t s1, std::uint64_t s2,
                 std::uint64_t s3) noexcept
{
    const Family family = Current();
    Count(MipRequests, family);
    if (Skips(MipRequests, family))
    {
        return false;
    }
    return reinterpret_cast<MipRequestsFn>(g_mipRequestsCall.Original())(commandList, a2, group, chunk, s1, s2, s3);
}

std::wstring BudgetState() noexcept
{
    std::uintptr_t managers = 0, me = 0;
    std::uint8_t   state    = 0;
    if (g_managers == 0 || !platform::CopyGuarded(&managers, reinterpret_cast<const void*>(g_managers), sizeof(managers)) ||
        managers == 0 || !platform::CopyGuarded(&me, reinterpret_cast<const void*>(managers + kMeManager), sizeof(me)) ||
        me == 0 || !platform::CopyGuarded(&state, reinterpret_cast<const void*>(me + kBudgetStateRender), sizeof(state)))
    {
        return L"?";
    }
    return std::to_wstring(state);
}

void RemoveAll() noexcept
{
    g_renderThreadPending.store(false, std::memory_order_release);
    std::atomic_ref<std::uint64_t>(g_shadowFields.skip).store(0, std::memory_order_relaxed);
    if (g_shadowCode.IsInstalled() && !g_shadowCode.Restore())
    {
        platform::Log(L"stream feedback: the shadow site was changed after us; left in place (it passes through)");
    }
    if (g_mipRequestsCall.IsInstalled())
    {
        g_mipRequestsCall.Restore();
    }
    if (g_hiddenEntry.IsInstalled())
    {
        g_hiddenEntry.Restore();
    }
    if (g_hiddenCall.IsInstalled())
    {
        g_hiddenCall.Restore();
    }
    if (g_copyEntry.IsInstalled())
    {
        g_copyEntry.Restore();
    }
    // 1.005's process and copy calls cross a block and were installed from the render thread: they stay, passing every
    // call through once Render no longer marks a family (this DLL is pinned).
    if (platform::RunningBuild() != platform::BuildIndex::V1005 && g_processCall.IsInstalled())
    {
        g_processCall.Restore();
    }
    g_render.Restore();
}

/// Game version 1.005 (kCopyCall above): the hidden pass's call now, Render, and the process and copy calls left to
/// the render thread's first Render. The functions were found (bytes checked) by the caller.
bool Install1005(const platform::GameImage& image, std::uintptr_t entry, std::uintptr_t render, std::uintptr_t call,
                 std::uintptr_t process, std::uintptr_t copy, std::uintptr_t mipCall, std::uintptr_t mip) noexcept
{
    const auto copyCall   = image.Rebase(kCopyCall, 5);
    const auto hiddenCall = image.Rebase(kHiddenCall, 5);
    const auto hidden     = platform::FindFunction(image, kHidden);
    if (!copyCall || !hiddenCall || !hidden)
    {
        platform::Log(L"stream feedback: the scenery streaming code is not what the research read; each family hands in "
                      L"requests as before");
        return false;
    }
    g_processSite   = call;
    g_processTarget = process;
    g_copySite      = *copyCall;
    g_copyTarget    = copy;
    g_renderThreadPending.store(true, std::memory_order_release);  // before Render can run
    const bool installed =
        g_hiddenCall.Install(*hiddenCall, *hidden, reinterpret_cast<std::uintptr_t>(&HiddenPass)) &&
        g_mipRequestsCall.Install(mipCall, mip, reinterpret_cast<std::uintptr_t>(&MipRequestsCall)) &&
        g_render.Replace(entry, render, reinterpret_cast<std::uintptr_t>(&Render));
    if (!installed)
    {
        RemoveAll();
        platform::Log(L"stream feedback: a site could not be redirected; each family hands in requests as before");
        return false;
    }
    g_on.store(true, std::memory_order_relaxed);
    InstallShadowSkip(image);
    g_installed.store(true, std::memory_order_release);
    platform::Log(L"stream feedback: installed; players 2 and 3's families skip the readback processing, the main pass's copy, "
                  L"the hidden-component pass and phase 8's mip-request dispatches, so player 1's family alone hands in "
                  L"scenery requests (players 2 and 3's "
                  L"come through their streaming views); the first two calls follow on the render thread");
    return true;
}

}  // namespace

bool InstallMeFeedbackOnce(const platform::GameImage& image) noexcept
{
    const auto entry   = image.Rebase(kRenderEntry, sizeof(void*));
    const auto render  = platform::FindFunction(image, kRenderMain);
    const auto call    = image.Rebase(kProcessCall, 5);
    const auto process = platform::FindFunction(image, kProcess);
    const auto copy    = platform::FindFunction(image, kCopy);
    const auto hidden  = platform::FindFunction(image, kHidden);
    const auto managers = image.Rebase(kStreamingManagers, sizeof(void*));
    const auto mipCall  = image.Rebase(kMipRequestsCall, 5);
    const auto mip      = platform::FindFunction(image, kMipRequests);
    if (!entry || !render || !call || !process || !copy || !hidden || !managers || !mipCall || !mip)
    {
        platform::Log(L"stream feedback: the scenery streaming code is not what the research read; each family hands in "
                      L"requests as before");
        return false;
    }
    g_managers = *managers;
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        return Install1005(image, *entry, *render, *call, *process, *copy, *mipCall, *mip);
    }
    // The skip points first: they pass everything through until Render marks a family.
    const bool installed =
        g_processCall.Install(*call, *process, reinterpret_cast<std::uintptr_t>(&Process)) &&
        g_copyEntry.Install(*copy, image.EntryInstruction(kCopy.address, kCopy.entryBytes.first(kCopyFirst)),
                            reinterpret_cast<std::uintptr_t>(&Copy)) &&
        g_hiddenEntry.Install(*hidden, image.EntryInstruction(kHidden.address, kHidden.entryBytes.first(kHiddenFirst)),
                            reinterpret_cast<std::uintptr_t>(&HiddenPass)) &&
        g_mipRequestsCall.Install(*mipCall, *mip, reinterpret_cast<std::uintptr_t>(&MipRequestsCall)) &&
        g_render.Replace(*entry, *render, reinterpret_cast<std::uintptr_t>(&Render));
    if (!installed)
    {
        RemoveAll();
        platform::Log(L"stream feedback: a site could not be redirected; each family hands in requests as before");
        return false;
    }
    g_on.store(true, std::memory_order_relaxed);
    InstallShadowSkip(image);
    g_installed.store(true, std::memory_order_release);
    platform::Log(L"stream feedback: installed; players 2 and 3's families skip the readback processing, the main pass's copy, "
                  L"the hidden-component pass and phase 8's mip-request dispatches, so player 1's family alone hands in "
                  L"scenery requests (players 2 and 3's "
                  L"come through their streaming views)");
    return true;
}

void RemoveMeFeedbackOnce() noexcept
{
    if (!MeFeedbackOnceInstalled())
    {
        return;
    }
    g_on.store(false, std::memory_order_relaxed);
    UpdateShadowSwitch();
    RemoveAll();
    g_installed.store(false, std::memory_order_release);
}

bool MeFeedbackOnceInstalled() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

void SetMeFeedbackOnce(bool playerOneOnly) noexcept
{
    g_on.store(playerOneOnly && MeFeedbackOnceInstalled(), std::memory_order_relaxed);
    UpdateShadowSwitch();
    platform::Log(L"stream feedback: {}", g_on.load(std::memory_order_relaxed)
                                              ? L"player 1's family alone hands in requests"
                                              : L"each family hands in requests (the game's code as it runs per family)");
}

void SetMeFeedbackMask(std::uint32_t mask) noexcept
{
    g_mask.store(mask & 0x1F, std::memory_order_relaxed);
    UpdateShadowSwitch();
    platform::Log(L"stream feedback: under p1, players 2 and 3's families skip: readback processing {}, main-pass copy {}, "
                  L"hidden-component pass {}, phase 8 mip requests {}, shadow streaming writes {}",
                  (mask & 1) ? L"yes" : L"no", (mask & 2) ? L"yes" : L"no", (mask & 4) ? L"yes" : L"no",
                  (mask & 8) ? L"yes" : L"no", (mask & 16) ? L"yes" : L"no");
}

std::uint32_t MeFeedbackMask() noexcept
{
    return g_mask.load(std::memory_order_relaxed);
}

bool MeFeedbackOnceOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void PollMeFeedbackOnce() noexcept
{
    if (!MeFeedbackOnceInstalled())
    {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_logAt)
    {
        return;
    }
    g_logAt = now + kLogPeriodMs;
    std::uint64_t delta[kStages][kFamilies] = {};
    bool          additionalRendered        = false;
    for (int s = 0; s < kStages; ++s)
    {
        for (int f = 0; f < kFamilies; ++f)
        {
            const std::uint64_t count = g_counts[s][f].load(std::memory_order_relaxed);
            delta[s][f]               = count - g_logged[s][f];
            g_logged[s][f]            = count;
        }
    }
    additionalRendered = delta[Renders][AdditionalPassed] + delta[Renders][AdditionalSkipped] != 0;
    if (!additionalRendered || !platform::Enabled(platform::Level::Debug))
    {
        return;  // one family per frame: nothing to report (or the periodic Debug line is not written)
    }
    auto line = [&](Stage stage) {
        return std::format(L"{} / {} passed / {} skipped", delta[stage][PlayerOne], delta[stage][AdditionalPassed],
                           delta[stage][AdditionalSkipped]);
    };
    const std::uint64_t shadow[3] = {ShadowCount(g_shadowFields.skipped), ShadowCount(g_shadowFields.passedOne),
                                     ShadowCount(g_shadowFields.passedAdditional)};
    const auto shadowLine = std::format(L"{} / {} passed / {} skipped", shadow[1] - g_shadowLogged[1],
                                        shadow[2] - g_shadowLogged[2], shadow[0] - g_shadowLogged[0]);
    std::copy(std::begin(shadow), std::end(shadow), g_shadowLogged);
    platform::LogDebug(L"stream feedback ({}, mask {}): in the last period, player 1's family / players 2 and 3's: renders {}; "
                  L"readback processing {}; main-pass copies {}; hidden-component passes {}; phase 8 chunk calls {}; "
                  L"shadow batches' streaming writes {}{}; budget state {}",
                  MeFeedbackOnceOn() ? L"p1" : L"each", g_mask.load(std::memory_order_relaxed), line(Renders),
                  line(Processes), line(Copies), line(Hidden), line(MipRequests), shadowLine,
                  g_shadowCode.IsInstalled() ? L"" : L" (not installed)", BudgetState());
}

#if LSS_DEV

void RegisterMeFeedbackCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"stream-feedback",
                       .shape = control::Shape::Word,
                       .words = {L"p1", L"each"},
                       .help  = L"which view families hand the game's scenery streaming their detail requests: player "
                                L"1's only, as with one player (p1, at start), or each family",
                       .set   = [](const Value& v) {
                           const bool playerOneOnly = std::get<std::wstring>(v) == L"p1";
                           if (!MeFeedbackOnceInstalled())
                           {
                               return Outcome{false, L"the one-copy streaming feedback is not installed (see the "
                                                     L"install log)"};
                           }
                           SetMeFeedbackOnce(playerOneOnly);
                           return Outcome{true, playerOneOnly ? L"player 1's family alone hands in streaming requests"
                                                              : L"each family hands in streaming requests (the game's "
                                                                L"code as it runs per family)"};
                       },
                       .get = [] { return Value{std::wstring(MeFeedbackOnceOn() ? L"p1" : L"each")}; }});
    control::Register({.name  = L"feedback-mask",
                       .shape = control::Shape::Integer,
                       .max   = 31,
                       .help  = L"diagnosis: which of players 2 and 3's skips under stream-feedback p1 act (1 the readback "
                                L"processing, 2 the main pass's copy, 4 the hidden-component pass, 8 the phase 8 "
                                L"mip-request dispatches, 16 the shadow pass's streaming writes)",
                       .set   = [](const Value& v) {
                           if (!MeFeedbackOnceInstalled())
                           {
                               return Outcome{false, L"the one-copy streaming feedback is not installed (see the "
                                                     L"install log)"};
                           }
                           SetMeFeedbackMask(static_cast<std::uint32_t>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"feedback mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{MeFeedbackMask()}}; }});
}

#endif

}  // namespace lss::renderer
