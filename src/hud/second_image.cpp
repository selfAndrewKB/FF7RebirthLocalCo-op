#include "hud/second_image.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/NameTypes.hpp>

#include "hud/area_icons_two.h"
#include "hud/enemy_gauges.h"
#include "hud/target_views.h"
#include "hud/entry_filter_code.h"
#include "hud/hud_addresses.h"
#include "hud/menu_buffers.h"
#include "hud/minimap_once.h"
#include "hud/minimap.h"
#include "hud/player_two_pass.h"
#include "hud/prompt_rows.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "player_two/generation.h"
#include "player_two/command_menu_two.h"

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
constexpr std::uintptr_t kRenderToCompositeLayer = 0x140C95CC0;
constexpr std::uint8_t   kRenderToCompositeLayerPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
                                                              0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0xB8, 0x80, 0x03, 0x00, 0x00};
constexpr std::uint32_t  kOverlayWindow = 0x2F0;

// Its call to FEndMenuRenderer__AcquireLayerSlot(renderer, pool 2): rcx = renderer, dl = 2, the
// slot in rax.
constexpr std::uintptr_t kAcquireCall = 0x140C960A2;
constexpr std::uintptr_t kAcquireLayerSlot = 0x140FC8694;
constexpr std::uint8_t   kAcquireLayerSlotPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};

// FApp::DeltaTime, a double, which the overlay's paint args and fade read.
constexpr std::uintptr_t kDeltaTime = 0x146FAA330;

// SceneRenderer_Render_Main's call to AddRenderCompositeLayerDelegatePasses(module, builder,
// &Views, texture, context), once per view family, before that family's composite consume.
constexpr std::uintptr_t kAddCompositePassesCall = 0x140FDE3C1;
constexpr std::uintptr_t kAddCompositePasses     = 0x140D1791C;
constexpr std::uint8_t   kAddCompositePassesPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                          0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr std::uint32_t  kViewUnscaledViewRect = 0x238;  // FIntRect in FSceneView (FViewInfo is a copy)

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
constexpr std::uintptr_t kVertexBufferVtable  = 0x146621CF8;
constexpr std::uintptr_t kIndexBufferVtable   = 0x146621D50;
constexpr std::uintptr_t kBeginInitResource   = 0x1409E7EF8;  // void (FRenderResource*)
constexpr std::uint8_t   kBeginInitResourcePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48,
                                                         0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0xB8, 0x90, 0x00, 0x00, 0x00};

constexpr const wchar_t* kPlayerTwoEntryName = L"SEndRootOverlay_P2";
constexpr const wchar_t* kStockEntryName     = L"SEndRootOverlay";
constexpr int            kFamilyLogLines     = 6;

using RenderToCompositeLayerFn = void (*)(void* overlay);
using AcquireFn                = void* (*)(void* renderer, std::uintptr_t pool);
using AddCompositePassesFn     = void* (*)(void* module, void* builder, const void* views, void* texture, void* context);
using SubmitLayerSlotFn        = void (*)(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused);

constexpr std::uintptr_t kPoolTwo = 2;  // the root overlay's pool (the dl argument at 0x140C960A2 and 0x140C96258)

enum class Pass
{
    None,
    PlayerTwo,
    PlayerOne,
};

// Installed once, then read.
platform::CallRedirect g_acquireCall;
platform::CallRedirect g_familyCall;
platform::CodeRedirect g_buffersCode;
platform::CodeRedirect g_gateCode;
bool                   g_installed          = false;
std::uintptr_t         g_overlaySingleton   = 0;
std::uintptr_t         g_deltaTime          = 0;
RenderToCompositeLayerFn g_renderToLayer    = nullptr;
BeginInitResourceFn    g_beginInitResource  = nullptr;
SubmitLayerSlotFn      g_submitLayerSlot    = nullptr;
std::uintptr_t         g_vertexBufferVtable = 0;
std::uintptr_t         g_indexBufferVtable  = 0;

// Read by the render-thread code (hud/entry_filter_code.h) through their addresses.
EntryFilterFields g_fields;
MenuBufferSet     g_buffers;  // player 2's (hud/menu_buffers.h)

// Game thread.
std::atomic<bool> g_on{false};
std::atomic<bool> g_imageThisFrame{false};  // player 2's entry was rendered this frame (read by the render thread)
Pass              g_pass          = Pass::None;
void*             g_renderer      = nullptr;  // from any acquire; the renderer is a long-lived singleton
void*             g_slot          = nullptr;  // player 2's pass's slot, for player 1's pass
std::atomic<std::uint64_t> g_stockName{0};  // as the game passes it; read by the render thread
std::uint64_t     g_playerTwoName = 0;
bool              g_buffersReady  = false;
bool              g_refused       = false;    // a check failed: the image stays off until restart

std::atomic<int> g_familyLogLines{0};

// Render cost, logged every kRenderTimingFrames frames while player 2's image is on.
constexpr std::uint32_t kRenderTimingFrames = 240;
double        g_secondsPerCount    = 0.0;
double        g_playerTwoSeconds   = 0.0;
double        g_playerOneSeconds   = 0.0;
double        g_playerOneStart     = 0.0;
std::uint32_t g_renderTimedFrames  = 0;

double Now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (g_secondsPerCount == 0.0)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        g_secondsPerCount = 1.0 / static_cast<double>(frequency.QuadPart);
    }
    return static_cast<double>(counter.QuadPart) * g_secondsPerCount;
}

using platform::CopyGuarded;

void Refuse(const wchar_t* why)
{
    g_refused = true;
    g_on.store(false, std::memory_order_relaxed);
    platform::Log(L"HUD image: {}; player 2's image stays off", why);
}

/// Builds player 2's buffer set the way the renderer's ctor builds each pool's, once the renderer
/// is known, and queues their initialisation. The vtables are the ones pool 2's own buffers carry.
void QueueBuffers() noexcept
{
    if (g_buffersReady || g_renderer == nullptr)  // already queued, or no acquire seen yet
    {
        return;
    }
    if (!BuildMenuBuffers(g_renderer, kPoolTwoVertexBuffer, kPoolTwoIndexBuffer, g_vertexBufferVtable,
                          g_indexBufferVtable, g_beginInitResource, g_buffers))
    {
        Refuse(L"pool 2's buffers are not the renderer's buffer kind the research read");
        return;
    }
    g_buffersReady = true;
    platform::Log(L"HUD image: player 2's vertex and index buffers created (capacity {} / {}); waiting for the render "
                  L"thread to initialise them", kMenuVertexCapacity, kMenuIndexCapacity);
}

/// Player 2's buffers exist and are initialised; publishes them to the render-thread code once. The render thread
/// runs up to a frame behind, so a slot holding player 2's entry must not be submitted before then: an
/// earlier-queued consume would draw it with an uninitialised buffer (seen: "Tried to call DrawIndexedPrimitive
/// with null IndexBuffer!").
bool BuffersUsable() noexcept
{
    if (!g_buffersReady)
    {
        QueueBuffers();
        return false;
    }
    if (!MenuBuffersInitialised(g_buffers))  // every frame (hud/menu_buffers.h)
    {
        return false;
    }
    if (g_fields.playerTwoName != 0)
    {
        return true;
    }
    g_fields.vertexBuffer = reinterpret_cast<std::uint64_t>(&g_buffers.vertex);
    g_fields.indexBuffer  = reinterpret_cast<std::uint64_t>(&g_buffers.index);
    g_fields.srmSlot      = reinterpret_cast<std::uint64_t>(&g_buffers.srm);
    std::atomic_thread_fence(std::memory_order_release);
    g_fields.playerTwoName = g_playerTwoName;  // last: the buffers code switches on this
    platform::Log(L"HUD image: player 2's buffers are initialised; rendering player 2's image");
    return true;
}

/// Replaces the overlay's call to AcquireLayerSlot.
void* AcquireLayerSlot(void* renderer, std::uintptr_t pool) noexcept
{
    g_renderer = renderer;
    if (g_pass == Pass::PlayerOne && g_slot != nullptr)
    {
        void* slot = g_slot;  // player 2's pass took this frame's slot
        g_slot     = nullptr;
        return slot;
    }
    void* slot = reinterpret_cast<AcquireFn>(g_acquireCall.Original())(renderer, pool);
    if (g_pass == Pass::PlayerTwo)
    {
        g_slot = slot;
    }
    return slot;
}

/// Replaces the scene renderer's call that adds a family's composite passes (render thread):
/// decides which entry this family draws, from its view's rect.
void* AddCompositePasses(void* module, void* builder, const void* views, void* texture, void* context) noexcept
{
    std::uint64_t family = 0;
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
            family               = playerTwo ? g_fields.playerTwoName : stockName;
            if (g_familyLogLines.fetch_add(1, std::memory_order_relaxed) < kFamilyLogLines)
            {
                platform::Log(L"HUD image: a family with view ({},{})-({},{}) draws player {}'s entry", rect[0], rect[1],
                              rect[2], rect[3], playerTwo ? 2 : 1);
            }
        }
    }
    g_fields.familyName = family;
    return reinterpret_cast<AddCompositePassesFn>(g_familyCall.Original())(module, builder, views, texture, context);
}

bool Redirect(const platform::GameImage& image, platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target,
              std::uintptr_t replacement) noexcept
{
    const auto live   = image.Rebase(site, 5);
    const auto rebased = image.Rebase(target);
    return live && rebased && call.Install(*live, *rebased, replacement);
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
    const bool prologues = image.BytesMatch(kRenderToCompositeLayer, kRenderToCompositeLayerPrologue) &&
                           image.BytesMatch(kAcquireLayerSlot, kAcquireLayerSlotPrologue) &&
                           image.BytesMatch(kAddCompositePasses, kAddCompositePassesPrologue) &&
                           image.BytesMatch(kBeginInitResource, kBeginInitResourcePrologue) &&
                           image.BytesMatch(engine::kSubmitLayerSlot, engine::kSubmitLayerSlotPrologue);
    const auto render    = image.Rebase(kRenderToCompositeLayer);
    const auto init      = image.Rebase(kBeginInitResource);
    const auto submit    = image.Rebase(engine::kSubmitLayerSlot);
    const auto overlay   = image.Rebase(engine::kRootOverlaySingleton, sizeof(void*));
    const auto delta     = image.Rebase(kDeltaTime, sizeof(double));
    const auto vbVtable  = image.Rebase(kVertexBufferVtable, sizeof(void*));
    const auto ibVtable  = image.Rebase(kIndexBufferVtable, sizeof(void*));
    if (!prologues || !render || !init || !submit || !overlay || !delta || !vbVtable || !ibVtable)
    {
        platform::Log(L"HUD image: the overlay's render code is not what the research read; no player-2 image");
        return false;
    }
    g_renderToLayer      = reinterpret_cast<RenderToCompositeLayerFn>(*render);
    g_beginInitResource  = reinterpret_cast<BeginInitResourceFn>(*init);
    g_submitLayerSlot    = reinterpret_cast<SubmitLayerSlotFn>(*submit);
    g_overlaySingleton   = *overlay;
    g_deltaTime          = *delta;
    g_vertexBufferVtable = *vbVtable;
    g_indexBufferVtable  = *ibVtable;

    const auto buffersCode = EntryBuffersCode(g_fields);
    const auto gateCode    = EntryDrawGateCode(g_fields);
    // The render-thread sites first: they do nothing until a family name and player 2's buffers are set.
    const bool installed =
        Patch(image, g_gateCode, kDrawGateSite, kDrawGateBytes, gateCode, kDrawGateResume) &&
        Patch(image, g_buffersCode, kLoadVertexBufferSite, kLoadVertexBufferBytes, buffersCode, kLoadVertexBufferResume) &&
        Redirect(image, g_familyCall, kAddCompositePassesCall, kAddCompositePasses,
                 reinterpret_cast<std::uintptr_t>(&AddCompositePasses)) &&
        Redirect(image, g_acquireCall, kAcquireCall, kAcquireLayerSlot, reinterpret_cast<std::uintptr_t>(&AcquireLayerSlot));
    if (!installed)
    {
        RemoveSecondImage();
        platform::Log(L"HUD image: a site is not what the research read; no player-2 image");
        return false;
    }
    // The render-thread code reads g_fields, which lives in this DLL: it must never unload while that
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
    g_fields.familyName    = 0;
    g_fields.playerTwoName = 0;
    bool all = true;
    for (platform::CallRedirect* call : {&g_acquireCall, &g_familyCall})
    {
        all = (!call->IsInstalled() || call->Restore()) && all;
    }
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
    g_pass = Pass::None;
    g_slot = nullptr;
    const bool image = splitShown && SecondImageOn() && g_stockName.load(std::memory_order_relaxed) != 0 && BuffersUsable();
    std::uintptr_t overlay = 0;
    std::uintptr_t window  = 0;
    if (image && CopyGuarded(&overlay, reinterpret_cast<const void*>(g_overlaySingleton), sizeof(overlay)) && overlay != 0 &&
        CopyGuarded(&window, reinterpret_cast<const void*>(overlay + kOverlayWindow), sizeof(window)) && window != 0)
    {
        double deltaTime = 0.0;
        std::memcpy(&deltaTime, reinterpret_cast<const void*>(g_deltaTime), sizeof(deltaTime));
        const double still = 0.0;
        std::memcpy(reinterpret_cast<void*>(g_deltaTime), &still, sizeof(still));
        g_pass              = Pass::PlayerTwo;
        prompt_rows::ShowOnly(1);                 // player 2's prompt rows only (hud/prompt_rows.h)
        player_two::command_menu_two::ShowOnly(1);  // in battle, player 2's command menu in player 2's image
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
        prompt_rows::ShowOnly(0);  // player 1's rows for player 1's pass
        player_two::command_menu_two::ShowOnly(0);  // and player 1's command menu
        std::memcpy(reinterpret_cast<void*>(g_deltaTime), &deltaTime, sizeof(deltaTime));
    }
    g_imageThisFrame.store(g_slot != nullptr, std::memory_order_relaxed);
    g_pass           = Pass::PlayerOne;
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
    if (g_slot != nullptr)
    {
        // Player 1's pass did not take the slot (its render returned early). The acquire has
        // already advanced the ring, so the slot is submitted as the game's own pass would have,
        // holding player 2's entry alone.
        g_submitLayerSlot(g_renderer, kPoolTwo, g_slot, 0);
        platform::Log(L"HUD image: player 1's pass did not render this frame; player 2's slot submitted on its own");
        g_slot = nullptr;
    }
    minimap::EndOverlayRender();  // the minimap's held pool-0 slot, if player 1's pass did not take it
    g_pass = Pass::None;
}

std::uintptr_t EntryNameForPass(std::uintptr_t stockName) noexcept
{
    if (g_pass != Pass::PlayerTwo)
    {
        if (g_stockName.load(std::memory_order_relaxed) == 0 && stockName != 0 && !g_refused)  // once, not per frame
        {
            // First sight of the game's name: check that names built here match the engine's.
            try
            {
                g_playerTwoName = EngineNameValue(kPlayerTwoEntryName, RC::Unreal::FNAME_Add);
                if (EngineNameValue(kStockEntryName, RC::Unreal::FNAME_Find) != stockName || g_playerTwoName == 0 ||
                    g_playerTwoName == stockName)
                {
                    Refuse(L"an FName built by the mod does not match the engine's layout");
                }
                else
                {
                    g_fields.stockName = stockName;  // before any family name can be set from g_stockName
                    g_stockName.store(stockName, std::memory_order_release);
                }
            }
            catch (...)
            {
                Refuse(L"the player-2 entry name could not be made");
            }
        }
        return stockName;
    }
    return static_cast<std::uintptr_t>(g_playerTwoName);
}

bool SubmitInThisPass() noexcept
{
    return g_pass != Pass::PlayerTwo;
}

void* MenuRenderer() noexcept
{
    return g_renderer;
}

}  // namespace lss::hud
