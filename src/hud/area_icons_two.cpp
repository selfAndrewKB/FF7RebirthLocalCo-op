#include "hud/area_icons_two.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <array>
#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::area_icons_two {

namespace {

// Facts read from the database (RE-p2-hud-icon-manager.md §1, §4 and §8; this build). Each
// function's prologue up to its stack probe call is checked before it is called or redirected.
constexpr std::uintptr_t kInstanceOne = 0x149349880;  // the game's instance (EndUISystem+0x1A0), a pointer
constexpr std::size_t    kInstanceSize = 0x160;

constexpr std::uintptr_t kMalloc           = 0x14083F3D8;  // FMemory_Malloc(rcx = size); never reads rdx
constexpr std::uint8_t   kMallocPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kCtor             = 0x143A0D944;  // AreaMapIconMgr__Ctor(memory), returns it
constexpr std::uint8_t   kCtorPrologue[]   = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kFrameUpdate      = 0x1409F72B8;  // AreaMapIcon_FrameUpdate(instance, xmm1 = DeltaTime)
constexpr std::uint8_t   kFrameUpdatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41,
                                                   0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x50, 0x07, 0x00, 0x00};
constexpr std::uintptr_t kClearAll         = 0x140F5D854;  // AreaMapIconMgr__ClearAllAndDisable(instance)
constexpr std::uint8_t   kClearAllPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57,
                                                0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0xB8};

// The frame function's only call for the first player controller (§8.2): E8 rel32 at 0x1409F7439
// to UWorld__GetFirstPlayerController, rcx = the world, inside the block at 0x1409F7430.
constexpr std::uintptr_t kFirstControllerCall = 0x1409F7439;
constexpr std::uintptr_t kFirstController     = 0x140EF3078;
constexpr std::uint8_t   kFirstControllerPrologue[] = {0xB8, 0x28, 0x00, 0x00, 0x00};

// SWidget::SetVisibility(widget, TAttribute<EVisibility>*), canvas vtable slot 59 (§8.3).
constexpr std::uintptr_t kSetVisibility           = 0x140C27B38;
constexpr std::uint8_t   kSetVisibilityPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr std::size_t    kSetVisibilitySlot       = 59;

// The instance (§1, §2): the canvases as TWeakPtr {object, reference controller} (#1 icons, #2 quest
// and party markers), the icon and marker maps, and the fields game events write.
constexpr std::array<std::uint32_t, 2> kCanvases{0x10, 0x70};
constexpr std::array<std::uint32_t, 2> kMaps{0x20, 0x80};
constexpr std::uint32_t kEnabled   = 0xD0;  // u8
constexpr std::uint32_t kFlagD1    = 0xD1;  // u8
constexpr std::uint32_t kCountdown = 0xDC;  // 4 bytes
constexpr std::uint32_t kSharedCount = 0x8;  // the weak pointer's reference controller: its shared count (int32)

// The canvas's Visibility attribute (§8.3): +0 value, +1 set, +8 getter delegate, +0x10 its size.
constexpr std::uint32_t kVisibility         = 0x1B0;
constexpr std::uint32_t kVisibilityDelegate = 0x1B8;
constexpr std::uint8_t  kHidden               = 0x02;
constexpr std::uint8_t  kSelfHitTestInvisible = 0x11;  // what the canvases are constructed with

constexpr int kMaxLines = 60;

enum Part : unsigned
{
    kPartTick       = 1,
    kPartController = 2,
    kPartCanvases   = 4,
};

struct VisibilityAttribute  // TAttribute<EVisibility> as the game passes it: unbound
{
    std::uint8_t  value = 0;
    std::uint8_t  isSet = 1;
    std::uint8_t  pad[6]{};
    std::uint64_t delegate = 0;
    std::int32_t  delegateSize = 0;
    std::int32_t  pad2 = 0;
};
static_assert(sizeof(VisibilityAttribute) == 24);

struct ShownCanvas  // a canvas of player 1's hidden for player 2's pass, and its own visibility
{
    std::uintptr_t canvas = 0;
    std::uint8_t   visibility = 0;
};

using MallocFn        = void* (*)(std::size_t size);
using CtorFn          = void* (*)(void* memory);
using FrameUpdateFn   = void (*)(void* instance, float deltaTime);
using ClearAllFn      = void (*)(void* instance);
using FirstControllerFn = void* (*)(void* world);
using SetVisibilityFn = void (*)(void* widget, VisibilityAttribute* attribute);

// Set once at install.
platform::CallRedirect g_firstControllerCall;
bool                   g_installed      = false;
std::uintptr_t         g_instanceOne    = 0;  // the address holding the game's instance
MallocFn               g_malloc         = nullptr;
CtorFn                 g_ctor           = nullptr;
FrameUpdateFn          g_frameUpdate    = nullptr;
ClearAllFn             g_clearAll       = nullptr;
SetVisibilityFn        g_setVisibility  = nullptr;

// Game thread.
std::uintptr_t               g_instanceTwo = 0;
bool                         g_createFailed = false;
bool                         g_tickFailed   = false;
void*                        g_playerTwoController = nullptr;  // answered while player 2's tick runs
unsigned                     g_mask   = kPartTick | kPartController | kPartCanvases;
std::array<ShownCanvas, 2>   g_hiddenOne{};
std::uint32_t                g_answered = 0;  // first-controller calls answered with player 2's
std::array<std::uint64_t, 7> g_logged{};
int                          g_lines = 0;
bool                         g_boundLogged = false;

bool Acts(Part part) noexcept
{
    return (g_mask & part) != 0;
}

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

/// Replaces the frame function's call for the first player controller.
void* FirstController(void* world) noexcept
{
    if (g_playerTwoController != nullptr)
    {
        ++g_answered;
        return g_playerTwoController;
    }
    return reinterpret_cast<FirstControllerFn>(g_firstControllerCall.Original())(world);
}

std::uintptr_t CreateGuarded() noexcept
{
    __try
    {
        void* memory = g_malloc(kInstanceSize);
        return memory == nullptr ? 0 : reinterpret_cast<std::uintptr_t>(g_ctor(memory));
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return 0;
    }
}

bool FrameUpdateGuarded(std::uintptr_t instance, float deltaTime) noexcept
{
    __try
    {
        g_frameUpdate(reinterpret_cast<void*>(instance), deltaTime);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool ClearAllGuarded(std::uintptr_t instance) noexcept
{
    __try
    {
        g_clearAll(reinterpret_cast<void*>(instance));
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool SetVisibilityGuarded(std::uintptr_t canvas, std::uint8_t value) noexcept
{
    __try
    {
        VisibilityAttribute attribute;
        attribute.value = value;
        g_setVisibility(reinterpret_cast<void*>(canvas), &attribute);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

std::uintptr_t InstanceOne() noexcept
{
    std::uintptr_t instance = 0;
    Read(g_instanceOne, instance);
    return instance;
}

/// The canvas a weak pointer at `instance + offset` holds, or 0 if it does not pin.
std::uintptr_t Canvas(std::uintptr_t instance, std::uint32_t offset) noexcept
{
    std::uintptr_t object     = 0;
    std::uintptr_t reference  = 0;  // the weak pointer's reference controller
    std::int32_t   shared     = 0;
    if (instance == 0 || !Read(instance + offset, object) || !Read(instance + offset + 8, reference) || object == 0 ||
        reference == 0 || !Read(reference + kSharedCount, shared) || shared <= 0)
    {
        return 0;
    }
    return object;
}

/// Whether the canvas's visibility can be set as the game sets it: unbound, and its vtable's slot is
/// SWidget::SetVisibility.
bool Settable(std::uintptr_t canvas) noexcept
{
    std::uintptr_t vtable   = 0;
    std::uintptr_t setter   = 0;
    std::uint64_t  delegate = 0;
    if (!Read(canvas, vtable) || !Read(vtable + kSetVisibilitySlot * sizeof(void*), setter) ||
        !Read(canvas + kVisibilityDelegate, delegate))
    {
        return false;
    }
    if (setter != reinterpret_cast<std::uintptr_t>(g_setVisibility) || delegate != 0)
    {
        if (!g_boundLogged)
        {
            g_boundLogged = true;
            platform::Log(L"area icons: a canvas is not the kind the research read (bound visibility or another "
                          L"setter); it is left shown in both halves");
        }
        return false;
    }
    return true;
}

std::uint8_t VisibilityOf(std::uintptr_t canvas) noexcept
{
    std::uint8_t value = kSelfHitTestInvisible;
    Read(canvas + kVisibility, value);
    return value;
}

void SetCanvas(std::uintptr_t canvas, std::uint8_t value) noexcept
{
    if (canvas != 0 && VisibilityOf(canvas) != value && Settable(canvas))
    {
        SetVisibilityGuarded(canvas, value);
    }
}

bool EnsureInstanceTwo() noexcept
{
    if (g_instanceTwo != 0 || g_createFailed)
    {
        return g_instanceTwo != 0;
    }
    g_instanceTwo = CreateGuarded();
    g_createFailed = g_instanceTwo == 0;
    platform::Log(L"area icons: player 2's icon manager {}", g_createFailed ? L"could not be created; none for player 2"
                                                                            : L"created with the game's constructor");
    return g_instanceTwo != 0;
}

/// The event-written fields of player 1's instance, copied into player 2's; a disable goes through
/// the game's own clear, as the game's hide event does.
void CopyEventState(std::uintptr_t one) noexcept
{
    std::uint8_t  enabledOne = 0, enabledTwo = 0, flag = 0;
    std::uint32_t countdown = 0;
    if (!Read(one + kEnabled, enabledOne) || !Read(g_instanceTwo + kEnabled, enabledTwo) || !Read(one + kFlagD1, flag) ||
        !Read(one + kCountdown, countdown))
    {
        return;
    }
    platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kFlagD1), &flag, sizeof(flag));
    platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kCountdown), &countdown, sizeof(countdown));
    if (enabledOne != 0 && enabledTwo == 0)
    {
        const std::uint8_t on = 1;
        platform::WriteGuarded(reinterpret_cast<void*>(g_instanceTwo + kEnabled), &on, sizeof(on));
    }
    else if (enabledOne == 0 && enabledTwo != 0)
    {
        ClearAllGuarded(g_instanceTwo);
    }
}

/// One line whenever either instance's enable flag, canvases or map sizes change.
void LogIfChanged(std::uintptr_t one) noexcept
{
    if (g_lines >= kMaxLines)
    {
        return;
    }
    std::array<std::uint64_t, 7> now{};
    std::uint8_t  enabled[2]{};
    std::int32_t  slots[2][2]{};
    std::uintptr_t canvases[2][2]{};
    const std::uintptr_t instances[2] = {one, g_instanceTwo};
    for (int i = 0; i < 2; ++i)
    {
        Read(instances[i] + kEnabled, enabled[i]);
        for (int k = 0; k < 2; ++k)
        {
            Read(instances[i] + kMaps[k] + 8, slots[i][k]);  // the map's element array count
            canvases[i][k] = Canvas(instances[i], kCanvases[k]);
        }
    }
    now = {enabled[0] | (std::uint64_t{enabled[1]} << 8), static_cast<std::uint32_t>(slots[0][0]),
           static_cast<std::uint32_t>(slots[0][1]), static_cast<std::uint32_t>(slots[1][0]),
           static_cast<std::uint32_t>(slots[1][1]), canvases[1][0], canvases[1][1]};
    if (now == g_logged)
    {
        return;
    }
    g_logged = now;
    ++g_lines;
    try
    {
        platform::Log(L"area icons: player 1's enabled {} icons/markers slots {}/{}; player 2's enabled {} slots {}/{} "
                      L"canvases {:#x}/{:#x}; first-controller answers {}",
                      enabled[0], slots[0][0], slots[0][1], enabled[1], slots[1][0], slots[1][1], canvases[1][0],
                      canvases[1][1], g_answered);
    }
    catch (...)
    {
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const bool prologues = image.BytesMatch(kMalloc, kMallocPrologue) && image.BytesMatch(kCtor, kCtorPrologue) &&
                           image.BytesMatch(kFrameUpdate, kFrameUpdatePrologue) &&
                           image.BytesMatch(kClearAll, kClearAllPrologue) &&
                           image.BytesMatch(kFirstController, kFirstControllerPrologue) &&
                           image.BytesMatch(kSetVisibility, kSetVisibilityPrologue);
    const auto instance   = image.Rebase(kInstanceOne, sizeof(void*));
    const auto allocate   = image.Rebase(kMalloc);
    const auto ctor       = image.Rebase(kCtor);
    const auto update     = image.Rebase(kFrameUpdate);
    const auto clear      = image.Rebase(kClearAll);
    const auto visibility = image.Rebase(kSetVisibility);
    const auto site       = image.Rebase(kFirstControllerCall, 5);
    const auto first      = image.Rebase(kFirstController);
    if (!prologues || !instance || !allocate || !ctor || !update || !clear || !visibility || !site || !first ||
        !g_firstControllerCall.Install(*site, *first, reinterpret_cast<std::uintptr_t>(&FirstController)))
    {
        platform::Log(L"area icons: the icon manager's code is not what the research read; no icons of player 2's own");
        return false;
    }
    g_instanceOne   = *instance;
    g_malloc        = reinterpret_cast<MallocFn>(*allocate);
    g_ctor          = reinterpret_cast<CtorFn>(*ctor);
    g_frameUpdate   = reinterpret_cast<FrameUpdateFn>(*update);
    g_clearAll      = reinterpret_cast<ClearAllFn>(*clear);
    g_setVisibility = reinterpret_cast<SetVisibilityFn>(*visibility);
    g_installed     = true;
    platform::Log(L"area icons: ready (player 2's icon manager is made when player 2 first plays)");
    return true;
}

void Remove() noexcept
{
    g_hiddenOne           = {};  // no Slate calls here: this may run off the game thread (PlayerTwoOut restored them)
    g_playerTwoController = nullptr;
    if (g_firstControllerCall.IsInstalled() && !g_firstControllerCall.Restore())
    {
        platform::Log(L"area icons: the first-controller call was changed after us; left in place (it passes through)");
    }
    g_installed = false;
}

void Tick(float deltaTime) noexcept
{
    const std::uintptr_t one = g_installed && Acts(kPartTick) && !g_tickFailed ? InstanceOne() : 0;
    if (one == 0 || !EnsureInstanceTwo())
    {
        return;
    }
    CopyEventState(one);
    g_playerTwoController = Acts(kPartController) ? player_two::generation::PlayerTwoController() : nullptr;
    const bool ticked     = FrameUpdateGuarded(g_instanceTwo, deltaTime);
    g_playerTwoController = nullptr;
    if (!ticked)
    {
        g_tickFailed = true;
        platform::Log(L"area icons: player 2's icon manager raised an exception; it is no longer ticked");
        return;
    }
    LogIfChanged(one);
}

void ShowOnly(int player) noexcept
{
    if (!g_installed || !Acts(kPartCanvases))
    {
        return;
    }
    const std::uintptr_t one = InstanceOne();
    for (std::size_t k = 0; k < kCanvases.size(); ++k)
    {
        // A hide still pending is undone first (a pass that did not finish), so a visibility this
        // module set is never recorded as the canvas's own. Only the canvas that was hidden gets it.
        if (g_hiddenOne[k].canvas != 0 && Canvas(one, kCanvases[k]) == g_hiddenOne[k].canvas)
        {
            SetCanvas(g_hiddenOne[k].canvas, g_hiddenOne[k].visibility);
        }
        g_hiddenOne[k]          = {};
        const std::uintptr_t two = Canvas(g_instanceTwo, kCanvases[k]);
        if (player == 0)
        {
            SetCanvas(two, kHidden);
            continue;
        }
        const std::uintptr_t canvas = Canvas(one, kCanvases[k]);
        const std::uint8_t   own    = canvas != 0 ? VisibilityOf(canvas) : kHidden;
        if (own != kHidden)  // one the game itself hides stays as the game has it
        {
            g_hiddenOne[k] = {canvas, own};
            SetCanvas(canvas, kHidden);
        }
        SetCanvas(two, kSelfHitTestInvisible);
    }
}

void PlayerTwoOut() noexcept
{
    ShowOnly(0);
    if (g_instanceTwo == 0)
    {
        return;
    }
    ClearAllGuarded(g_instanceTwo);
    for (const std::uint32_t offset : kCanvases)
    {
        SetCanvas(Canvas(g_instanceTwo, offset), kHidden);
    }
    platform::Log(L"area icons: player 2 is out; player 2's icon manager cleared");
}

void SetMask(unsigned mask) noexcept
{
    if (Acts(kPartCanvases) && (mask & kPartCanvases) == 0)
    {
        ShowOnly(0);  // player 1's canvases shown, as the game has them
        for (const std::uint32_t offset : kCanvases)
        {
            SetCanvas(Canvas(g_instanceTwo, offset), kSelfHitTestInvisible);  // and player 2's, in both halves
        }
    }
    g_mask  = mask;
    g_lines = 0;
    platform::Log(L"area icons: mask {} (ticked {}, first controller player 2's {}, canvases per pass {})", mask,
                  Acts(kPartTick), Acts(kPartController), Acts(kPartCanvases));
}

}  // namespace lss::hud::area_icons_two
