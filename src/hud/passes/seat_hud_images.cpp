#include "hud/passes/seat_hud_images.h"

#include "facts/engine.h"
#include "facts/hud.h"
#include "hud/passes/entry_filter_code.h"
#include "hud/passes/shared_slot.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"

#if LSS_DEV
#include "hud/passes/root_overlay_render.h"
#include "platform/command_registry.h"
#endif

#include <array>
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
// FSceneView::PlayerIndex, an int32: the local player's controller id, which is the seat (players/seats.h); the view's
// player whatever its place on screen (RE-scene-view-player-index.md).
constexpr std::uint32_t  kViewPlayerIndex      = 0x228;
// Game version 1.005 (RE-1005-hud-2.md): the call (0x140C1481D there) crosses a 16-byte block on the render thread, so
// the function's entry is redirected instead; Render_Main's call is its only caller, so every call is still that one.
// First instruction `mov [rsp+8],rbx`.
constexpr std::size_t    kAddCompositePassesFirst = 5;

// The consume loop's sites (hud/passes/entry_filter_code.h), with the instruction after each.
constexpr std::uintptr_t kLoadVertexBufferSite   = 0x140FC8501;
constexpr std::uintptr_t kLoadVertexBufferResume = 0x140FC8508;
constexpr std::uintptr_t kDrawGateSite           = 0x140FC8E05;
constexpr std::uintptr_t kDrawGateResume         = 0x140FC8E0A;

// FEndMenuRenderer__Ctor's per-pool buffers (pool 2's are passed to the consume): the vertex buffer object at
// renderer+0x90, the index buffer at +0xA8 (hud/passes/menu_buffers.h), initialised through
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
std::uintptr_t         g_frameNumber        = 0;  // GFrameNumber (facts::kFrameNumber), held one back per further pass
RenderToCompositeLayerFn g_renderToLayer    = nullptr;

// Each further seat's entry in the overlay's pool-2 slot (hud/passes/shared_slot.h).
SharedLayerSlot g_slot{{L"HUD image", kPoolTwo, kPoolTwoVertexBuffer, kPoolTwoIndexBuffer, L"SEndRootOverlay",
                        {nullptr, L"SEndRootOverlay_P2", L"SEndRootOverlay_P3"}}};

// Game thread.
std::atomic<bool> g_on{false};
std::atomic<unsigned> g_imageSeats{0};      // bit `seat`: the seat's entry was rendered this frame (render thread reads)
std::atomic<std::uint64_t> g_stockName{0};  // as the game passes it; read by the render thread
bool              g_refused       = false;    // a check failed: the image stays off until restart
std::uintptr_t    g_overlayThisFrame = 0;     // the overlay with a window, found by ImageReady (0: none)
double            g_deltaTimeSaved   = 0.0;   // the engine's DeltaTime while a further pass holds it at 0
std::uint32_t     g_frameNumberSaved = 0;     // GFrameNumber while a further pass holds it one back

std::atomic<int> g_familyLogLines{0};

// Render thread: whose entry each player's view drew last (0 none, else the seat whose entry + 1), so each change is
// logged once at Info: the regression checks that each view draws its own player's HUD. Reset while no seat has one.
std::array<int, players::kMaxPlayers> g_viewDraws{};

using platform::CopyGuarded;

void Refuse(const wchar_t* why)
{
    g_refused = true;
    g_on.store(false, std::memory_order_relaxed);
    platform::Log(L"HUD image: {}; the further players' HUD image stays off", why);
}

/// Replaces the overlay's call to AcquireLayerSlot.
void* AcquireLayerSlot(void* renderer, std::uintptr_t pool) noexcept
{
    return g_slot.Acquire(renderer, pool, reinterpret_cast<AcquireLayerSlotFn>(g_acquireCall.Original()));
}

/// Replaces the scene renderer's call that adds a family's composite passes (render thread):
/// decides which entry this family draws, from its view's player: a seat with an entry this frame draws its own, every
/// other view (player 1's, a view with no seat) player 1's.
void* AddCompositePasses(void* module, void* builder, const void* views, void* texture, void* context) noexcept
{
    EntryFilterFields&  fields    = g_slot.Fields();
    std::uint64_t       family    = 0;
    const std::uint64_t stockName = g_stockName.load(std::memory_order_acquire);
    const unsigned      seats     = g_imageSeats.load(std::memory_order_relaxed);
    if (seats == 0)
    {
        g_viewDraws = {};  // the next image's views are logged afresh
    }
    if (seats != 0 && stockName != 0 && views != nullptr)
    {
        struct ViewArray
        {
            const std::uint8_t* data;
            std::int32_t        num;
        } array{};
        std::int32_t rect[4] = {};
        std::int32_t player  = -1;
        if (CopyGuarded(&array, views, sizeof(array)) && array.num == 1 && array.data != nullptr &&
            CopyGuarded(&player, array.data + kViewPlayerIndex, sizeof(player)))
        {
            const bool own = players::SeatIndex(player) && (seats & (1u << player)) != 0;
            family         = own ? fields.Seat(player).name : stockName;
            if (player >= 0 && player < players::kMaxPlayers)
            {
                const int draws = (own ? player : 0) + 1;
                if (g_viewDraws[static_cast<std::size_t>(player)] != draws)
                {
                    g_viewDraws[static_cast<std::size_t>(player)] = draws;
                    platform::Log(L"HUD image: {}'s view draws {}'s HUD", players::SeatName(player),
                                  players::SeatName(draws - 1));
                }
            }
            if (platform::Enabled(platform::Level::Debug) &&
                g_familyLogLines.fetch_add(1, std::memory_order_relaxed) < kFamilyLogLines &&
                CopyGuarded(rect, array.data + kViewUnscaledViewRect, sizeof(rect)))
            {
                platform::LogDebug(L"HUD image: a family with view ({},{})-({},{}), player index {}, draws {}'s entry",
                                   rect[0], rect[1], rect[2], rect[3], player, players::SeatName(own ? player : 0));
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
    const auto frame     = image.Rebase(facts::kFrameNumber, sizeof(std::uint32_t));
    const auto vbVtable  = image.Rebase(facts::kVertexBufferVtable, sizeof(void*));
    const auto ibVtable  = image.Rebase(facts::kIndexBufferVtable, sizeof(void*));
    if (!render || !passes || !acquire || !init || !submit || !overlay || !delta || !frame || !vbVtable || !ibVtable)
    {
        platform::Log(L"HUD image: the overlay's render code is not what the research read; no image for the further "
                      L"players");
        return false;
    }
    g_renderToLayer    = reinterpret_cast<RenderToCompositeLayerFn>(*render);
    g_overlaySingleton = *overlay;
    g_deltaTime        = *delta;
    g_frameNumber      = *frame;
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
    // The render-thread sites first: they do nothing until a family name and a seat's buffers are set.
    const bool installed =
        Patch(image, g_gateCode, kDrawGateSite, kDrawGateBytes, gateCode, kDrawGateResume) &&
        Patch(image, g_buffersCode, kLoadVertexBufferSite, kLoadVertexBufferBytes, buffersCode, kLoadVertexBufferResume) &&
        familyRedirect() &&
        g_acquireCall.InstallResearch(image, kAcquireCall, facts::kAcquireLayerSlot.address,
                                      reinterpret_cast<std::uintptr_t>(&AcquireLayerSlot));
    if (!installed)
    {
        RemoveSecondImage();
        platform::Log(L"HUD image: a site is not what the research read; no image for the further players");
        return false;
    }
    // The render-thread code reads the slot's fields, which live in this DLL: it must never unload while that
    // code can run. (The call redirects above pin it too; this does not rely on that.)
    if (!platform::PinThisModule())
    {
        RemoveSecondImage();
        platform::Log(L"HUD image: this DLL could not be pinned; no image for the further players");
        return false;
    }
    g_installed = true;
    g_on.store(!g_refused, std::memory_order_relaxed);  // on from the start; the hud-image command switches it
    return true;
}

void RemoveSecondImage() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_imageSeats.store(0, std::memory_order_relaxed);
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
    platform::Log(L"HUD image: the further players' images {}", SecondImageOn() ? L"on" : on ? L"unavailable" : L"off");
}

bool SecondImageOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void BeginFrame() noexcept
{
    g_slot.BeginFrame();
    g_overlayThisFrame = 0;
}

bool ImageReady(int seat, bool shown) noexcept
{
    bool image = shown && SecondImageOn() && g_stockName.load(std::memory_order_relaxed) != 0;
    if (image)
    {
        const auto buffers = g_slot.BuffersUsable(seat);
        if (buffers == SharedLayerSlot::Buffers::WrongKind)
        {
            Refuse(L"pool 2's buffers are not the renderer's buffer kind the research read");
        }
        image = buffers == SharedLayerSlot::Buffers::Usable;
    }
    std::uintptr_t overlay = 0;
    std::uintptr_t window  = 0;
    g_overlayThisFrame     = 0;
    if (image && CopyGuarded(&overlay, reinterpret_cast<const void*>(g_overlaySingleton), sizeof(overlay)) && overlay != 0 &&
        CopyGuarded(&window, reinterpret_cast<const void*>(overlay + kOverlayWindow), sizeof(window)) && window != 0)
    {
        g_overlayThisFrame = overlay;
    }
    return g_overlayThisFrame != 0;
}

double HoldFrame() noexcept
{
    double deltaTime = 0.0;
    std::memcpy(&deltaTime, reinterpret_cast<const void*>(g_deltaTime), sizeof(deltaTime));
    const double still = 0.0;
    std::memcpy(reinterpret_cast<void*>(g_deltaTime), &still, sizeof(still));
    g_deltaTimeSaved = deltaTime;
    // The frame number one back for the pass (RE-damage-numbers-per-view.md §5): every battle popup stamps the frame
    // it was painted or caught up in, and the overlay's post-paint catch-up advances only the widgets whose stamp is
    // not this frame's. A further pass stamping this frame would hide from player 1's catch-up (the real frame time,
    // last) every number player 1 did not paint, which then neither rises nor expires while player 1 looks away. A
    // worker thread reading the global inside the pass would see the frame before [ASSUMPTION: none acts on it].
    std::uint32_t frame = 0;
    std::memcpy(&frame, reinterpret_cast<const void*>(g_frameNumber), sizeof(frame));
    g_frameNumberSaved       = frame;
    const std::uint32_t back = frame - 1;
    std::memcpy(reinterpret_cast<void*>(g_frameNumber), &back, sizeof(back));
    return deltaTime;
}

void ReleaseFrame() noexcept
{
    std::memcpy(reinterpret_cast<void*>(g_deltaTime), &g_deltaTimeSaved, sizeof(g_deltaTimeSaved));
    std::memcpy(reinterpret_cast<void*>(g_frameNumber), &g_frameNumberSaved, sizeof(g_frameNumberSaved));
}

double BeginFurtherPass() noexcept
{
    const double deltaTime = HoldFrame();
    g_slot.SetActive(true);
    return deltaTime;
}

bool RenderFurther() noexcept
{
    // Guarded like every other engine call the mod makes: on a fault the minimap's target field and held slot
    // are still put back by the driver (a held pool-0 slot would hang its next acquire), and the images stop.
    const bool rendered = platform::CallGuarded(g_renderToLayer, reinterpret_cast<void*>(g_overlayThisFrame));
    if (!rendered)
    {
        Refuse(L"a further player's overlay render raised an exception");
    }
    return rendered;
}

void EndFurtherPass() noexcept
{
    ReleaseFrame();
    g_slot.SetActive(false);
}

void BeginPlayerOnePass() noexcept
{
    g_imageSeats.store(g_slot.Held() ? g_slot.EntrySeats() : 0u, std::memory_order_relaxed);
}

bool ImageThisFrame() noexcept
{
    return g_imageSeats.load(std::memory_order_relaxed) != 0;
}

void EndFrame() noexcept
{
    if (g_slot.SubmitIfLeft())
    {
        // Player 1's pass did not take the slot (its render returned early); it was submitted as the game's own
        // pass would have, holding the further players' entries alone. Info the first time (an unusual path), Debug
        // after: a state that lasts (a cutscene) would repeat it per frame.
        static bool s_said = false;
        platform::LogAt(s_said ? platform::Level::Debug : platform::Level::Info,
                        L"HUD image: player 1's pass did not render this frame; the further players' slot submitted on "
                        L"its own");
        s_said = true;
    }
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

#if LSS_DEV

void RegisterSecondImageCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register(
        {.name  = L"hud-image",
         .shape = control::Shape::OnOff,
         .help  = L"the further players' HUD images: the overlay rendered once more per further player and drawn "
                  L"only in that player's view",
         .set   = [](const Value& v) {
             if (!SecondImageInstalled() || !RootOverlayRenderInstalled())
             {
                 return Outcome{false, L"the further players' HUD images are not installed (see the startup log)"};
             }
             const bool on = std::get<bool>(v);
             SetSecondImage(on);
             if (on && !SecondImageOn())
             {
                 return Outcome{false, L"the further players' HUD images refused a check (see the log)"};
             }
             return Outcome{true, on ? L"the further players' HUD images on" : L"the further players' HUD images off"};
         },
         .get = [] { return Value{SecondImageOn()}; }});
}

#endif

}  // namespace lss::hud
