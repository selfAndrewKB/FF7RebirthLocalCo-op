#include "renderer/me_feedback_once.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"

#include <atomic>
#include <cstdint>
#include <format>
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
constexpr std::uintptr_t kRenderMain  = 0x140FDD1F0;  // SceneRenderer_Render_Main(renderer, RHI command list)
constexpr std::uint8_t   kRenderMainBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                               0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

// The gate's call of ME_Group_ProcessStreamingFeedbackReadbacks(group), its only caller.
constexpr std::uintptr_t kProcessCall = 0x140915E38;
constexpr std::uintptr_t kProcess     = 0x1409156B4;
constexpr std::uint8_t   kProcessBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};

// ME_MainPass_CopyStreamingFeedbackToReadback(builder, scene): its one call (0x14157B30C) crosses a
// 16-byte block, so its entry is redirected; first instruction `mov [rsp+10h],rdx`.
constexpr std::uintptr_t kCopy = 0x140915958;
constexpr std::uint8_t   kCopyBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C,
                                         0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54};
constexpr std::size_t    kCopyFirst   = 5;

// ME_Scene_ComputeRequestedMips_HiddenComponents(renderer, command list, &Views[0]): the same; first
// instruction `mov [rsp+18h],r8`.
constexpr std::uintptr_t kHidden = 0x140CF05F8;
constexpr std::uint8_t   kHiddenBytes[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54,
                                           0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54};
constexpr std::size_t    kHiddenFirst   = 5;

constexpr std::uint32_t kRendererAdditionalFamily = 0x84;  // the family copy's bAdditionalViewFamily (+0x74)

// The budget state, for the log: IStreamingManager's static (FStreamingManagerCollection*), its ME
// streaming manager at +0x50, the render thread's copy of the state at +0xA1 (2 or more: no slave requests).
constexpr std::uintptr_t kStreamingManagers = 0x149117AF8;
constexpr std::uint32_t  kMeManager         = 0x50;
constexpr std::uint32_t  kBudgetStateRender = 0xA1;

constexpr ULONGLONG kLogPeriodMs = 10'000;

using RenderFn  = void (*)(void* renderer, void* commandList, void* a3, void* a4);
using ProcessFn = void (*)(void* group);
using CopyFn    = void (*)(void* builder, void* scene);
using HiddenFn  = void (*)(void* renderer, void* commandList, const void* views);

platform::VTableEntry   g_render;
platform::CallRedirect  g_processCall;
platform::EntryRedirect g_copyEntry;
platform::EntryRedirect g_hiddenEntry;
std::atomic<bool>       g_installed{false};
std::atomic<bool>       g_on{false};
std::uintptr_t          g_managers = 0;  // live address of the static

enum Family : int
{
    PlayerOne = 0,   // player 1's family (or any render that is not player 2's)
    TwoPassed = 1,   // player 2's family, switch off
    TwoSkipped = 2,  // player 2's family, the call skipped
    kFamilies = 3,
};

enum Stage : int
{
    Renders = 0,
    Processes = 1,
    Copies = 2,
    Hidden = 3,
    kStages = 4,
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

void Count(Stage stage, Family family) noexcept
{
    g_counts[stage][family].fetch_add(1, std::memory_order_relaxed);
}

/// Replaces the scene renderer's Render (vtable slot): marks player 2's family for its length.
void Render(void* renderer, void* commandList, void* a3, void* a4) noexcept
{
    std::uint8_t additional = 0;
    platform::CopyGuarded(&additional, static_cast<const std::uint8_t*>(renderer) + kRendererAdditionalFamily, 1);
    const Family previous = t_family;  // a render nested inside another on this thread restores the outer one
    t_family              = additional == 0                            ? PlayerOne
                            : g_on.load(std::memory_order_relaxed)     ? TwoSkipped
                                                                       : TwoPassed;
    Count(Renders, t_family);
    reinterpret_cast<RenderFn>(g_render.Original())(renderer, commandList, a3, a4);
    t_family = previous;
}

/// Replaces the gate's call of the readback processing.
void Process(void* group) noexcept
{
    const Family family = Current();
    Count(Processes, family);
    if (family != TwoSkipped)
    {
        reinterpret_cast<ProcessFn>(g_processCall.Original())(group);
    }
}

/// Replaces the main pass's feedback copy (entry).
void Copy(void* builder, void* scene) noexcept
{
    const Family family = Current();
    Count(Copies, family);
    if (family != TwoSkipped)
    {
        reinterpret_cast<CopyFn>(g_copyEntry.Original())(builder, scene);
    }
}

/// Replaces the hidden-component pass (entry).
void HiddenPass(void* renderer, void* commandList, const void* views) noexcept
{
    const Family family = Current();
    Count(Hidden, family);
    if (family != TwoSkipped)
    {
        reinterpret_cast<HiddenFn>(g_hiddenEntry.Original())(renderer, commandList, views);
    }
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
    if (g_hiddenEntry.IsInstalled())
    {
        g_hiddenEntry.Restore();
    }
    if (g_copyEntry.IsInstalled())
    {
        g_copyEntry.Restore();
    }
    if (g_processCall.IsInstalled())
    {
        g_processCall.Restore();
    }
    g_render.Restore();
}

}  // namespace

bool InstallMeFeedbackOnce(const platform::GameImage& image) noexcept
{
    const auto entry   = image.Rebase(kRenderEntry, sizeof(void*));
    const auto render  = image.Rebase(kRenderMain);
    const auto call    = image.Rebase(kProcessCall, 5);
    const auto process = image.Rebase(kProcess);
    const auto copy    = image.Rebase(kCopy);
    const auto hidden  = image.Rebase(kHidden);
    const auto managers = image.Rebase(kStreamingManagers, sizeof(void*));
    if (!entry || !render || !call || !process || !copy || !hidden || !managers ||
        !image.BytesMatch(kRenderMain, kRenderMainBytes) || !image.BytesMatch(kProcess, kProcessBytes) ||
        !image.BytesMatch(kCopy, kCopyBytes) || !image.BytesMatch(kHidden, kHiddenBytes))
    {
        platform::Log(L"stream feedback: the scenery streaming code is not what the research read; each family hands in "
                      L"requests as before");
        return false;
    }
    g_managers = *managers;
    // The skip points first: they pass everything through until Render marks a family.
    const bool installed =
        g_processCall.Install(*call, *process, reinterpret_cast<std::uintptr_t>(&Process)) &&
        g_copyEntry.Install(*copy, std::span(kCopyBytes, kCopyFirst), reinterpret_cast<std::uintptr_t>(&Copy)) &&
        g_hiddenEntry.Install(*hidden, std::span(kHiddenBytes, kHiddenFirst), reinterpret_cast<std::uintptr_t>(&HiddenPass)) &&
        g_render.Replace(*entry, *render, reinterpret_cast<std::uintptr_t>(&Render));
    if (!installed)
    {
        RemoveAll();
        platform::Log(L"stream feedback: a site could not be redirected; each family hands in requests as before");
        return false;
    }
    g_on.store(true, std::memory_order_relaxed);
    g_installed.store(true, std::memory_order_release);
    platform::Log(L"stream feedback: installed; player 2's family skips the readback processing, the main pass's copy "
                  L"and the hidden-component pass, so player 1's family alone hands in scenery requests (player 2's "
                  L"come through its streaming view)");
    return true;
}

void RemoveMeFeedbackOnce() noexcept
{
    if (!MeFeedbackOnceInstalled())
    {
        return;
    }
    g_on.store(false, std::memory_order_relaxed);
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
    platform::Log(L"stream feedback: {}", g_on.load(std::memory_order_relaxed)
                                              ? L"player 1's family alone hands in requests"
                                              : L"each family hands in requests (the game's code as it runs per family)");
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
    bool          familyTwo                 = false;
    for (int s = 0; s < kStages; ++s)
    {
        for (int f = 0; f < kFamilies; ++f)
        {
            const std::uint64_t count = g_counts[s][f].load(std::memory_order_relaxed);
            delta[s][f]               = count - g_logged[s][f];
            g_logged[s][f]            = count;
        }
    }
    familyTwo = delta[Renders][TwoPassed] + delta[Renders][TwoSkipped] != 0;
    if (!familyTwo || !platform::Enabled(platform::Level::Debug))
    {
        return;  // one family per frame: nothing to report (or the periodic Debug line is not written)
    }
    auto line = [&](Stage stage) {
        return std::format(L"{} / {} passed / {} skipped", delta[stage][PlayerOne], delta[stage][TwoPassed],
                           delta[stage][TwoSkipped]);
    };
    platform::LogDebug(L"stream feedback ({}): in the last period, player 1's family / player 2's: renders {}; readback "
                  L"processing {}; main-pass copies {}; hidden-component passes {}; budget state {}",
                  MeFeedbackOnceOn() ? L"p1" : L"each", line(Renders), line(Processes), line(Copies), line(Hidden),
                  BudgetState());
}

}  // namespace lss::renderer
