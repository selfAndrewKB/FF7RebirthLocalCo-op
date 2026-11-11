#include "hud/passes/column_hud_images.h"

#include "facts/engine.h"
#include "facts/hud.h"
#include "hud/passes/entry_filter_code.h"
#include "hud/passes/overlay_pass.h"
#include "hud/passes/shared_slot.h"
#include "hud/passes/ui_effect_pass.h"
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
#include <format>
#include <string>

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
// FSceneView::PlayerIndex, an int32: the local player's controller id (a seat's, or a mirror's after the seats'); the
// view's player whatever its place on screen (RE-scene-view-player-index.md). Its column comes from the columns map
// this frame published (g_viewColumns).
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
std::uintptr_t         g_frameNumber        = 0;  // GFrameNumber (facts::kFrameNumber), held one back in a further pass
RenderToCompositeLayerFn g_renderToLayer    = nullptr;

// Each further column's entry in the overlay's pool-2 slot (hud/passes/shared_slot.h).
SharedLayerSlot g_slot{{L"HUD image", kPoolTwo, kPoolTwoVertexBuffer, kPoolTwoIndexBuffer, L"SEndRootOverlay",
                        {L"SEndRootOverlay_Column2", L"SEndRootOverlay_Column3"}}};

// Game thread.
std::atomic<bool> g_on{false};
// The render thread's read of this frame, one 64-bit value so every part is of one frame:
//   bits 0..23   the view-to-column table: for controller id `id` (a seat's or a mirror's), 4 bits at 4 * id holding
//                its column + 1 (0: not listed);
//   bits 24..35  whose HUD each column's entry shows: 4 bits at 24 + 4 * column holding the seat + 1;
//   bits 40..47  bit 40 + `column`: the column's entry was rendered;
//   bits 48..55  bit 48 + `column`: that entry is player 1's HUD laid out for the column's view.
constexpr int kViewIds = players::kFirstMirrorId + players::kMirrorIds;  // the ids a list can hold (players/seats.h)
static_assert(4 * kViewIds <= 24, "the view-to-column table fits its 24 bits");
static_assert(4 * views::columns::kMaxColumns <= 12 && views::columns::kMaxColumns <= 8, "the other parts fit theirs");
constexpr unsigned           kShowsShift   = 24;
constexpr unsigned           kEntriesShift = 40;
constexpr unsigned           kOnesShift    = 48;
std::atomic<std::uint64_t>   g_frameRead{0};
unsigned                     g_playerOnesThisFrame = 0;  // game thread: the further columns showing player 1's HUD
std::uint64_t                g_showsThisFrame      = 0;  // game thread, as the passes are planned (shifted already)

/// The rendered further columns' bits (bit `column`) of a frame's read.
unsigned EntryColumns(std::uint64_t read) noexcept
{
    return static_cast<unsigned>((read >> kEntriesShift) & 0xFF);
}

/// The column of the view whose player is `id`, by a frame's read; -1 when not listed.
int ViewColumn(std::uint64_t read, std::int32_t id) noexcept
{
    if (id < 0 || id >= kViewIds)
    {
        return -1;
    }
    return static_cast<int>((read >> (4 * id)) & 0xF) - 1;
}

/// A view for the log: a player's ("player 2's view"), the mirror view in its column, or a view the list does not hold.
std::wstring ViewName(std::int32_t id, int column)
{
    if (id >= 0 && id < players::kMaxPlayers)
    {
        return std::format(L"{}'s view", players::SeatName(id));
    }
    return column >= 0 ? std::format(L"the mirror view in column {}", column + 1)
                       : std::format(L"an unlisted view (controller id {})", id);
}
std::atomic<std::uint64_t> g_stockName{0};  // as the game passes it; read by the render thread
bool              g_refused       = false;    // a check failed: the image stays off until restart
std::uintptr_t    g_overlayThisFrame = 0;     // the overlay with a window, found by ImageReady (0: none)
double            g_deltaTimeSaved   = 0.0;   // the engine's DeltaTime while a further pass holds it at 0
std::uint32_t     g_frameNumberSaved = 0;     // GFrameNumber while a further pass holds it one back

std::atomic<int> g_familyLogLines{0};

// Render thread: what each view drew last, by its player's id (0 none, else the seat whose HUD + 1, or
// kDrawsPlayerOnes), so each change is logged once at Info: the regression checks that each view draws its own
// player's HUD. Reset while no further column has an entry.
std::array<int, kViewIds> g_viewDraws{};
constexpr int kDrawsPlayerOnes = players::kMaxPlayers + 1;  // the column's entry, holding player 1's HUD laid out for it

using platform::CopyGuarded;

void Refuse(const wchar_t* why)
{
    g_refused = true;
    g_on.store(false, std::memory_order_relaxed);
    platform::Log(L"HUD image: {}; players 2 and 3's HUD images stay off", why);
}

/// Replaces the overlay's call to AcquireLayerSlot.
void* AcquireLayerSlot(void* renderer, std::uintptr_t pool) noexcept
{
    return g_slot.Acquire(renderer, pool, reinterpret_cast<AcquireLayerSlotFn>(g_acquireCall.Original()));
}

/// Replaces the scene renderer's call that adds a family's composite passes (render thread):
/// decides which entry this family draws, from its view's column: a further column with an entry this frame draws its
/// own, every other view (column 0's, a column with no entry) column 0's.
void* AddCompositePasses(void* module, void* builder, const void* views, void* texture, void* context) noexcept
{
    EntryFilterFields&  fields    = g_slot.Fields();
    std::uint64_t       family    = 0;
    const std::uint64_t stockName = g_stockName.load(std::memory_order_acquire);
    const std::uint64_t read      = g_frameRead.load(std::memory_order_relaxed);  // every part of one frame
    const unsigned      columns   = EntryColumns(read);
    ui_effect_pass::NoteFamilyColumn(-1);  // no column until this family's is known below: no map meanwhile
    if (columns == 0)
    {
        g_viewDraws = {};  // the next image's views are logged afresh
    }
    if (columns != 0 && stockName != 0 && views != nullptr)
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
            const int  column     = ViewColumn(read, player);
            const bool own        = column >= 1 && column <= kFurtherColumns && (columns & (1u << column)) != 0;
            const bool playerOnes = own && ((read >> (kOnesShift + column)) & 1) != 0;
            const int  shows      = own ? static_cast<int>((read >> (kShowsShift + 4 * column)) & 0xF) - 1 : 0;
            family                = own ? fields.Column(column).name : stockName;
            // The UI particle pass that follows in this family maps through the band of the column whose entry it
            // draws: its own, or column 0's when it draws column 0's HUD (hud/passes/ui_effect_pass.h).
            ui_effect_pass::NoteFamilyColumn(own ? column : 0);
            if (player >= 0 && player < kViewIds)
            {
                const int draws = playerOnes ? kDrawsPlayerOnes : (shows >= 0 ? shows : 0) + 1;
                if (g_viewDraws[static_cast<std::size_t>(player)] != draws)
                {
                    g_viewDraws[static_cast<std::size_t>(player)] = draws;
                    try
                    {
                        if (playerOnes)
                        {
                            platform::Log(L"HUD image: {} draws player 1's HUD laid out for its own shape",
                                          ViewName(player, column));
                        }
                        else
                        {
                            platform::Log(L"HUD image: {} draws {}'s HUD", ViewName(player, column),
                                          players::SeatName(draws - 1));
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
            if (platform::Enabled(platform::Level::Debug) &&
                g_familyLogLines.fetch_add(1, std::memory_order_relaxed) < kFamilyLogLines &&
                CopyGuarded(rect, array.data + kViewUnscaledViewRect, sizeof(rect)))
            {
                platform::LogDebug(L"HUD image: a family with view ({},{})-({},{}), player index {}, column {}, draws "
                                   L"column {}'s entry",
                                   rect[0], rect[1], rect[2], rect[3], player, column + 1, own ? column + 1 : 1);
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

bool InstallColumnHudImages(const platform::GameImage& image) noexcept
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
        platform::Log(L"HUD image: the overlay's render code is not what the research read; no image for players 2 "
                      L"and 3");
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
        RemoveColumnHudImages();
        platform::Log(L"HUD image: a site is not what the research read; no image for players 2 and 3");
        return false;
    }
    // The render-thread code reads the slot's fields, which live in this DLL: it must never unload while that
    // code can run. (The call redirects above pin it too; this does not rely on that.)
    if (!platform::PinThisModule())
    {
        RemoveColumnHudImages();
        platform::Log(L"HUD image: this DLL could not be pinned; no image for players 2 and 3");
        return false;
    }
    g_installed = true;
    g_on.store(!g_refused, std::memory_order_relaxed);  // on from the start; the hud-image command switches it
    return true;
}

void RemoveColumnHudImages() noexcept
{
    g_on.store(false, std::memory_order_relaxed);
    g_frameRead.store(0, std::memory_order_relaxed);
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

bool ColumnHudImagesInstalled() noexcept
{
    return g_installed;
}

void SetColumnHudImages(bool on) noexcept
{
    g_on.store(on && g_installed && !g_refused, std::memory_order_relaxed);
    g_familyLogLines.store(0, std::memory_order_relaxed);
    platform::Log(L"HUD image: players 2 and 3's images {}",
                  ColumnHudImagesOn() ? L"on" : on ? L"unavailable" : L"off");
}

bool ColumnHudImagesOn() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void BeginFrame() noexcept
{
    g_slot.BeginFrame();
    g_overlayThisFrame    = 0;
    g_playerOnesThisFrame = 0;
    g_showsThisFrame      = 0;
}

bool ImageReady(int column, bool shown, int shownSeat) noexcept
{
    const bool further = column >= 1 && column <= kFurtherColumns;
    if (shown && further && shownSeat >= 0 && shownSeat < players::kMaxPlayers)
    {
        // Only read for a column whose entry renders.
        g_showsThisFrame |= static_cast<std::uint64_t>(shownSeat + 1) << (kShowsShift + 4 * column);
        if (shownSeat == 0)
        {
            g_playerOnesThisFrame |= 1u << column;
        }
    }
    bool image = shown && further && ColumnHudImagesOn() && g_stockName.load(std::memory_order_relaxed) != 0;
    if (image)
    {
        const auto buffers = g_slot.BuffersUsable(column);
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
    // not this frame's. A seat's pass stamping this frame would hide from player 1's catch-up (the real frame time,
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

double BeginColumnImage() noexcept
{
    const double deltaTime = HoldFrame();
    g_slot.SetActive(true);
    return deltaTime;
}

bool RenderColumnImage() noexcept
{
    // Guarded like every other engine call the mod makes: on a fault the minimap's target field and held slot
    // are still put back by the driver (a held pool-0 slot would hang its next acquire), and the images stop.
    const bool rendered = platform::CallGuarded(g_renderToLayer, reinterpret_cast<void*>(g_overlayThisFrame));
    if (!rendered)
    {
        Refuse(L"player 2's or 3's overlay render raised an exception");
    }
    return rendered;
}

void EndColumnImage() noexcept
{
    ReleaseFrame();
    g_slot.SetActive(false);
}

void BeginPlayerOnePass() noexcept
{
    // The frame's read in one value: the view-to-column table (an id listed twice keeps its first column, as the map
    // keeps a seat's), whose HUD each column shows, the rendered columns and which of them show player 1's.
    const views::columns::Map& map  = overlay_pass::Map();
    std::uint64_t              read = 0;
    for (int column = 0; column < map.count && column < views::columns::kMaxColumns; ++column)
    {
        const std::int32_t id = map.idAt[static_cast<std::size_t>(column)];
        if (id >= 0 && id < kViewIds && ViewColumn(read, id) < 0)
        {
            read |= static_cast<std::uint64_t>(column + 1) << (4 * id);
        }
    }
    const unsigned columns = g_slot.Held() ? g_slot.EntryColumns() & 0xFFu : 0u;
    read |= g_showsThisFrame;
    read |= static_cast<std::uint64_t>(columns) << kEntriesShift;
    read |= static_cast<std::uint64_t>(g_playerOnesThisFrame & columns) << kOnesShift;
    g_frameRead.store(read, std::memory_order_relaxed);
}

bool ImageThisFrame() noexcept
{
    return EntryColumns(g_frameRead.load(std::memory_order_relaxed)) != 0;
}

void EndFrame() noexcept
{
    if (g_slot.SubmitIfLeft())
    {
        // Player 1's pass did not take the slot (its render returned early); it was submitted as the game's own
        // pass would have, holding the seats' entries alone. Info the first time (an unusual path), Debug
        // after: a state that lasts (a cutscene) would repeat it per frame.
        static bool s_said = false;
        platform::LogAt(s_said ? platform::Level::Debug : platform::Level::Info,
                        L"HUD image: player 1's pass did not render this frame; players 2 and 3's slot submitted on "
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

void RegisterColumnHudImagesCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register(
        {.name  = L"hud-image",
         .shape = control::Shape::OnOff,
         .help  = L"players 2 and 3's HUD images: the overlay rendered once more for each of them and drawn "
                  L"only in that player's view",
         .set   = [](const Value& v) {
             if (!ColumnHudImagesInstalled() || !RootOverlayRenderInstalled())
             {
                 return Outcome{false, L"players 2 and 3's HUD images are not installed (see the startup log)"};
             }
             const bool on = std::get<bool>(v);
             SetColumnHudImages(on);
             if (on && !ColumnHudImagesOn())
             {
                 return Outcome{false, L"players 2 and 3's HUD images refused a check (see the log)"};
             }
             return Outcome{true, on ? L"players 2 and 3's HUD images on" : L"players 2 and 3's HUD images off"};
         },
         .get = [] { return Value{ColumnHudImagesOn()}; }});
}

#endif

}  // namespace lss::hud
