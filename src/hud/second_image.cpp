#include "hud/second_image.h"

#include "facts/hud.h"
#include "hud/area_icons_two.h"
#include "hud/enemy_gauges.h"
#include "hud/target_views.h"
#include "hud/entry_filter_code.h"
#include "hud/minimap_once.h"
#include "hud/minimap.h"
#include "hud/overlay_pass.h"
#include "hud/player_two_pass.h"
#include "hud/pass_opacity.h"
#include "hud/shared_slot.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"

#include <atomic>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

// Facts read from the database (RE-p2-hud-second-image.md §1.3, §3.3 and §9; RE-p2-hud.md P2.5).
// Each function's prologue up to its stack probe call is checked before it is called or redirected.

// SEndRootOverlay__RenderToCompositeLayer(overlay): returns at once while the overlay has no
// window (+0x2F0), before writing anything.
constexpr std::uint8_t   kRenderToCompositeLayerPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
                                                              0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0xB8, 0x80, 0x03, 0x00, 0x00};
constexpr platform::FunctionFact kRenderToCompositeLayer{L"SEndRootOverlay__RenderToCompositeLayer", 0x140C95CC0,
                                                         kRenderToCompositeLayerPrologue};
constexpr std::uint32_t  kOverlayWindow = 0x2F0;

// Its call to FEndMenuRenderer__AcquireLayerSlot(renderer, pool 2): rcx = renderer, dl = 2, the
// slot in rax.
constexpr std::uintptr_t kAcquireCall = 0x140C960A2;

// FApp::DeltaTime, a double, which the overlay's paint args and fade read.
constexpr std::uintptr_t kDeltaTime = 0x146FAA330;

// SceneRenderer_Render_Main's call to AddRenderCompositeLayerDelegatePasses(module, builder,
// &Views, texture, context), once per view family, before that family's composite consume.
constexpr std::uintptr_t kAddCompositePassesCall = 0x140FDE3C1;
constexpr std::uint8_t   kAddCompositePassesPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                          0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::FunctionFact kAddCompositePasses{L"SceneRenderer_AddCompositePasses", 0x140D1791C,
                                                     kAddCompositePassesPrologue};
constexpr std::uint32_t  kViewUnscaledViewRect = 0x238;  // FIntRect in FSceneView (FViewInfo is a copy)
// Game version 1.005 (RE-1005-hud-2.md): the call (0x140C1481D there) crosses a 16-byte block on the render thread, so
// the function's entry is redirected instead; Render_Main's call is its only caller, so every call is still that one.
// First instruction `mov [rsp+8],rbx`.
constexpr std::size_t    kAddCompositePassesFirst = 5;

// The consume loop's sites (hud/entry_filter_code.h), with the instruction after each.
constexpr std::uintptr_t kLoadVertexBufferSite   = 0x140FC8501;
constexpr std::uintptr_t kLoadVertexBufferResume = 0x140FC8508;
constexpr std::uintptr_t kDrawGateSite           = 0x140FC8E05;
constexpr std::uintptr_t kDrawGateResume         = 0x140FC8E0A;

// FEndMenuRenderer__Ctor's per-pool buffers (pool 2's are passed to the consume): the vertex buffer object at
// renderer+0x90, the index buffer at +0xA8 (hud/menu_buffers.h), initialised through
// BeginInitResource_OrDirectIfRenderThread (render thread: vt+0x28 directly).
constexpr std::uint32_t  kPoolTwoVertexBuffer = 0x90;
constexpr std::uint32_t  kPoolTwoIndexBuffer  = 0xA8;

constexpr int            kFamilyLogLines     = 6;

using RenderToCompositeLayerFn = void (*)(void* overlay);
using AddCompositePassesFn     = void* (*)(void* module, void* builder, const void* views, void* texture, void* context);

constexpr std::uintptr_t kPoolTwo = 2;  // the root overlay's pool (the dl argument at 0x140C960A2 and 0x140C96258)

// Installed once, then read.
platform::CallRedirect g_acquireCall;
platform::CallRedirect g_familyCall;
platform::EntryRedirect g_familyEntry;  // 1.005, in place of g_familyCall
platform::CodeRedirect g_buffersCode;
platform::CodeRedirect g_gateCode;
bool                   g_installed          = false;
std::uintptr_t         g_overlaySingleton   = 0;
std::uintptr_t         g_deltaTime          = 0;
RenderToCompositeLayerFn g_renderToLayer    = nullptr;

// Player 2's entry in the overlay's pool-2 slot (hud/shared_slot.h).
SharedLayerSlot g_slot{{L"HUD image", kPoolTwo, kPoolTwoVertexBuffer, kPoolTwoIndexBuffer, L"SEndRootOverlay",
                        L"SEndRootOverlay_P2"}};

// Game thread.
std::atomic<bool> g_on{false};
std::atomic<bool> g_imageThisFrame{false};  // player 2's entry was rendered this frame (read by the render thread)
std::atomic<std::uint64_t> g_stockName{0};  // as the game passes it; read by the render thread
bool              g_refused       = false;    // a check failed: the image stays off until restart

std::atomic<int> g_familyLogLines{0};

// Render cost, logged every kRenderTimingFrames frames while player 2's image is on.
constexpr std::uint32_t kRenderTimingFrames = 240;
double        g_playerTwoSeconds   = 0.0;
double        g_playerOneSeconds   = 0.0;
double        g_playerOneStart     = 0.0;
std::uint32_t g_renderTimedFrames  = 0;

using platform::frame_watch::Now;

using platform::CopyGuarded;

void Refuse(const wchar_t* why)
{
    g_refused = true;
    g_on.store(false, std::memory_order_relaxed);
    platform::Log(L"HUD image: {}; player 2's image stays off", why);
}

/// Replaces the overlay's call to AcquireLayerSlot.
void* AcquireLayerSlot(void* renderer, std::uintptr_t pool) noexcept
{
    return g_slot.Acquire(renderer, pool, reinterpret_cast<AcquireLayerSlotFn>(g_acquireCall.Original()));
}

/// Replaces the scene renderer's call that adds a family's composite passes (render thread):
/// decides which entry this family draws, from its view's rect.
void* AddCompositePasses(void* module, void* builder, const void* views, void* texture, void* context) noexcept
{
    EntryFilterFields&  fields    = g_slot.Fields();
    std::uint64_t       family    = 0;
    const std::uint64_t stockName = g_stockName.load(std::memory_order_acquire);
    if (g_imageThisFrame.load(std::memory_order_relaxed) && stockName != 0 && views != nullptr)
    {
        struct ViewArray
        {
            const std::uint8_t* data;
            std::int32_t        num;
        } array{};
        std::int32_t rect[4] = {};
        if (CopyGuarded(&array, views, sizeof(array)) && array.num == 1 && array.data != nullptr &&
            CopyGuarded(rect, array.data + kViewUnscaledViewRect, sizeof(rect)))
        {
            const bool playerTwo = rect[0] > 0 || rect[1] > 0;  // player 1's half starts at the origin
            family               = playerTwo ? fields.playerTwoName : stockName;
            if (platform::Enabled(platform::Level::Debug) &&
                g_familyLogLines.fetch_add(1, std::memory_order_relaxed) < kFamilyLogLines)
            {
                platform::LogDebug(L"HUD image: a family with view ({},{})-({},{}) draws player {}'s entry", rect[0],
                                   rect[1], rect[2], rect[3], playerTwo ? 2 : 1);
            }
        }
    }
    fields.familyName = family;
    // The entry redirect's trampoline is set before its first call can arrive; only one of the two is ever installed.
    const std::uintptr_t original = g_familyEntry.Original() != 0 ? g_familyEntry.Original() : g_familyCall.Original();
    return reinterpret_cast<AddCompositePassesFn>(original)(module, builder, views, texture, context);
}

bool Patch(const platform::GameImage& image, platform::CodeRedirect& redirect, std::uintptr_t site,
           std::span<const std::uint8_t> replaced, std::span<const std::uint8_t> code, std::uintptr_t resume) noexcept
{
    const auto live      = image.Rebase(site, replaced.size());
    const auto liveResume = image.Rebase(resume);
    return live && liveResume && redirect.Install(*live, replaced, code, *liveResume);
}

}  // namespace

bool InstallSecondImage(const platform::GameImage& image) noexcept
{
    const auto render    = platform::FindFunction(image, kRenderToCompositeLayer);
    const auto passes    = platform::FindFunction(image, kAddCompositePasses);
    const auto acquire   = platform::FindFunction(image, facts::kAcquireLayerSlot);
    const auto init      = platform::FindFunction(image, facts::kBeginInitResource);
    const auto submit    = platform::FindFunction(image, facts::kSubmitLayerSlot);
    const auto overlay   = image.Rebase(facts::kRootOverlaySingleton, sizeof(void*));
    const auto delta     = image.Rebase(kDeltaTime, sizeof(double));
    const auto vbVtable  = image.Rebase(facts::kVertexBufferVtable, sizeof(void*));
    const auto ibVtable  = image.Rebase(facts::kIndexBufferVtable, sizeof(void*));
    if (!render || !passes || !acquire || !init || !submit || !overlay || !delta || !vbVtable || !ibVtable)
    {
        platform::Log(L"HUD image: the overlay's render code is not what the research read; no player-2 image");
        return false;
    }
    g_renderToLayer    = reinterpret_cast<RenderToCompositeLayerFn>(*render);
    g_overlaySingleton = *overlay;
    g_deltaTime        = *delta;
    g_slot.Install(*vbVtable, *ibVtable, reinterpret_cast<BeginInitResourceFn>(*init),
                   reinterpret_cast<SubmitLayerSlotFn>(*submit));

    const auto buffersCode = EntryBuffersCode(g_slot.Fields());
    const auto gateCode    = EntryDrawGateCode(g_slot.Fields());
    // The family decision: the call on the research build, the function's entry on 1.005 (kAddCompositePassesFirst).
    const auto familyRedirect = [&image, passes = *passes]() noexcept {
        if (platform::RunningBuild() != platform::BuildIndex::V1005)
        {
            return g_familyCall.Install(image, kAddCompositePassesCall, passes,
                                        reinterpret_cast<std::uintptr_t>(&AddCompositePasses));
        }
        const auto first = kAddCompositePasses.entryBytes.first(kAddCompositePassesFirst);
        return g_familyEntry.Install(passes, image.EntryInstruction(kAddCompositePasses.address, first),
                                     reinterpret_cast<std::uintptr_t>(&AddCompositePasses));
    };
    // The render-thread sites first: they do nothing until a family name and player 2's buffers are set.
    const bool installed =
        Patch(image, g_gateCode, kDrawGateSite, kDrawGateBytes, gateCode, kDrawGateResume) &&
        Patch(image, g_buffersCode, kLoadVertexBufferSite, kLoadVertexBufferBytes, buffersCode, kLoadVertexBufferResume) &&
        familyRedirect() &&
        g_acquireCall.InstallResearch(image, kAcquireCall, facts::kAcquireLayerSlot.address,
                                      reinterpret_cast<std::uintptr_t>(&AcquireLayerSlot));
    if (!installed)
    {
        RemoveSecondImage();
        platform::Log(L"HUD image: a site is not what the research read; no player-2 image");
        return false;
    }
    // The render-thread code reads the slot's fields, which live in this DLL: it must never unload while that
    // code can run. (The call redirects above pin it too; this does not rely on that.)
    if (!platform::PinThisModule())
    {
        RemoveSecondImage();
        platform::Log(L"HUD image: this DLL could not be pinned; no player-2 image");
        return false;
    }
    g_installed = true;
    g_on.store(!g_refused, std::memory_order_relaxed);  // on from the start; the hud-image command switches it
    return true;
}

void RemoveSecondImage() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_imageThisFrame.store(false, std::memory_order_relaxed);
    g_slot.Unpublish();
    bool all = true;
    for (platform::CallRedirect* call : {&g_acquireCall, &g_familyCall})
    {
        all = (!call->IsInstalled() || call->Restore()) && all;
    }
    all = (!g_familyEntry.IsInstalled() || g_familyEntry.Restore()) && all;
    for (platform::CodeRedirect* code : {&g_buffersCode, &g_gateCode})
    {
        all = (!code->IsInstalled() || code->Restore()) && all;
    }
    if (!all)
    {
        platform::Log(L"HUD image: a site was changed after us; left in place (it passes through with the image off)");
    }
    g_installed = false;
}

bool SecondImageInstalled() noexcept
{
    return g_installed;
}

void SetSecondImage(bool on) noexcept
{
    g_on.store(on && g_installed && !g_refused, std::memory_order_relaxed);
    g_familyLogLines.store(0, std::memory_order_relaxed);
    platform::Log(L"HUD image: player 2's image {}", SecondImageOn() ? L"on" : on ? L"unavailable" : L"off");
}

bool SecondImageOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void BeginOverlayRender(bool splitShown) noexcept
{
    SetOverlayPass(OverlayPass::None);
    g_slot.BeginFrame(false);
    bool image = splitShown && SecondImageOn() && g_stockName.load(std::memory_order_relaxed) != 0;
    if (image)
    {
        const auto buffers = g_slot.BuffersUsable();
        if (buffers == SharedLayerSlot::Buffers::WrongKind)
        {
            Refuse(L"pool 2's buffers are not the renderer's buffer kind the research read");
        }
        image = buffers == SharedLayerSlot::Buffers::Usable;
    }
    std::uintptr_t overlay = 0;
    std::uintptr_t window  = 0;
    if (image && CopyGuarded(&overlay, reinterpret_cast<const void*>(g_overlaySingleton), sizeof(overlay)) && overlay != 0 &&
        CopyGuarded(&window, reinterpret_cast<const void*>(overlay + kOverlayWindow), sizeof(window)) && window != 0)
    {
        double deltaTime = 0.0;
        std::memcpy(&deltaTime, reinterpret_cast<const void*>(g_deltaTime), sizeof(deltaTime));
        const double still = 0.0;
        std::memcpy(reinterpret_cast<void*>(g_deltaTime), &still, sizeof(still));
        SetOverlayPass(OverlayPass::PlayerTwo);
        g_slot.BeginFrame(true);
        pass_opacity::ShowOnly(1);  // player 2's rows, markers, pins and command menu only (hud/pass_opacity.h)
        target_views::BeforePass(1);  // player 1's cursor and gauges collapsed before player 2's updates read them
        const bool content  = EnterPlayerTwo();  // player 2's levers and updates (hud/player_two_pass.h)
        if (content)
        {
            area_icons_two::Tick(static_cast<float>(deltaTime));  // under player 2's view rect (hud/area_icons_two.h)
        }
        area_icons_two::ShowOnly(1);
        const double start  = Now();
        // Player 2's minimap (hud/minimap.h); without it, player 1's pass alone renders the minimap's map
        // (hud/minimap_once.h).
        BeginMinimapPlayerTwoPass(minimap::BeginPlayerTwoPaint());
        // Guarded like every other engine call the mod makes: on a fault the minimap's target field and held slot
        // are still put back below (a held pool-0 slot would hang its next acquire), and player 2's image stops.
        const bool rendered = platform::CallGuarded(g_renderToLayer, reinterpret_cast<void*>(overlay));
        EndMinimapPlayerTwoPass();
        minimap::EndPlayerTwoPaint();
        if (!rendered)
        {
            Refuse(L"player 2's overlay render raised an exception");
        }
        g_playerTwoSeconds += Now() - start;
        enemy_gauges::EndPlayerTwoPass();  // each gauge's size back to player 1's before player 1's updates
        target_views::BeforePass(0);  // player 2's, collapsed before player 1's updates (put back after the render)
        if (content)
        {
            LeavePlayerTwo();
        }
        area_icons_two::ShowOnly(0);
        pass_opacity::ShowOnly(0);  // player 1's rows, markers, pins and command menu for player 1's pass
        std::memcpy(reinterpret_cast<void*>(g_deltaTime), &deltaTime, sizeof(deltaTime));
    }
    g_imageThisFrame.store(g_slot.Held(), std::memory_order_relaxed);
    SetOverlayPass(OverlayPass::PlayerOne);
    g_playerOneStart = Now();
}

void EndOverlayRender() noexcept
{
    if (g_imageThisFrame.load(std::memory_order_relaxed))
    {
        g_playerOneSeconds += Now() - g_playerOneStart;
        if (++g_renderTimedFrames >= kRenderTimingFrames)
        {
            if (platform::frame_watch::Detailed())  // the perf-log switch
            {
                const platform::frame_watch::Job job(L"HUD image timing log");
                const double perFrame = 1000.0 / g_renderTimedFrames;
                platform::Log(L"HUD image: overlay render ms per frame over {} frames: player 2's pass {:.3f}, player 1's pass {:.3f}",
                              g_renderTimedFrames, g_playerTwoSeconds * perFrame, g_playerOneSeconds * perFrame);
            }
            g_playerTwoSeconds  = 0.0;
            g_playerOneSeconds  = 0.0;
            g_renderTimedFrames = 0;
        }
    }
    if (g_slot.SubmitIfLeft())
    {
        // Player 1's pass did not take the slot (its render returned early); it was submitted as the game's own
        // pass would have, holding player 2's entry alone. Info the first time (an unusual path), Debug after: a
        // state that lasts (a cutscene) would repeat it per frame.
        static bool s_said = false;
        platform::LogAt(s_said ? platform::Level::Debug : platform::Level::Info,
                        L"HUD image: player 1's pass did not render this frame; player 2's slot submitted on its own");
        s_said = true;
    }
    minimap::EndOverlayRender();  // the minimap's held pool-0 slot, if player 1's pass did not take it
    SetOverlayPass(OverlayPass::None);
}

std::uintptr_t EntryNameForPass(const void* slot, std::uintptr_t stockName) noexcept
{
    if (!g_slot.NamesReady() && stockName != 0 && !g_refused)  // once: the first sight of the game's name
    {
        if (g_slot.MakeNames(stockName))
        {
            g_stockName.store(stockName, std::memory_order_release);
        }
        else
        {
            Refuse(L"an FName built by the mod does not match the engine's layout, or could not be made");
        }
    }
    return g_slot.NameFor(slot, stockName);
}

bool SubmitInThisPass(const void* slot) noexcept
{
    return !g_slot.SkipSubmit(slot);
}

}  // namespace lss::hud
