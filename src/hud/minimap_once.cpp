#include "hud/minimap_once.h"

#include "facts/hud.h"
#include "hud/minimap_once_code.h"
#include "platform/build_value.h"
#include "platform/code_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"

#include <cstdint>
#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

// Facts read from the database (RE-oom-path.md, "Player 2's minimap paint: hook facts and the skip"; this
// build). Each widget: the replaced instruction, the acquire and submit calls the skip jumps over (checked
// so the block is the one the research read) and the instruction after the submit, where it lands.
struct WidgetSite
{
    const wchar_t*                 name;
    std::uintptr_t                 site;
    std::span<const std::uint8_t>  siteBytes;
    std::uintptr_t                 acquireCall;
    std::span<const std::uint8_t>  acquireBytes;
    std::uintptr_t                 submitCall;
    std::span<const std::uint8_t>  submitBytes;
    std::uintptr_t                 afterSubmit;
    std::span<const std::uint8_t>  afterSubmitBytes;
};

constexpr std::uint8_t kCircleAcquire[]      = {0xE8, 0xE1, 0x19, 0x00, 0x00};  // call FEndMenuRenderer__AcquireLayerSlot
constexpr std::uint8_t kCircleSubmit[]       = {0xE8, 0x24, 0x18, 0x00, 0x00};  // call FEndMenuRenderer__SubmitLayerSlot
constexpr std::uint8_t kCircleAfterSubmit[]  = {0x49, 0x8B, 0x8F, 0xE0, 0x01, 0x00, 0x00};  // mov rcx,[r15+1E0h]
constexpr std::uint8_t kOctagonAcquire[]     = {0xE8, 0x44, 0x84, 0x56, 0xFD};
constexpr std::uint8_t kOctagonSubmit[]      = {0xE8, 0x57, 0x82, 0x56, 0xFD};
constexpr std::uint8_t kOctagonAfterSubmit[] = {0x49, 0x8B, 0x8E, 0xE0, 0x01, 0x00, 0x00};  // mov rcx,[r14+1E0h]

constexpr WidgetSite kCircle{L"circle", 0x140FC6C95, kCircleSiteBytes, facts::kNaviMapCircleAcquireCall, kCircleAcquire,
                             facts::kNaviMapCircleSubmitCall, kCircleSubmit, 0x140FC6F18, kCircleAfterSubmit};
constexpr WidgetSite kOctagon{L"octagon", 0x143A60235, kOctagonSiteBytes, 0x143A6024B, kOctagonAcquire,
                              0x143A604E0, kOctagonSubmit, 0x143A604E5, kOctagonAfterSubmit};
// Game version 1.005 (RE-1005-hud-2.md): the circle's site is the next instruction, 0x140FC6C9C here (the same block
// and landing point; its own skip code, hud/minimap_once_code.h).
constexpr WidgetSite kCircle1005{L"circle", 0x140FC6C9C, kCircleSiteBytes1005, facts::kNaviMapCircleAcquireCall,
                                 kCircleAcquire, facts::kNaviMapCircleSubmitCall, kCircleSubmit, 0x140FC6F18,
                                 kCircleAfterSubmit};

constexpr ULONGLONG kLogPeriodMs = 60'000;  // the counts line, once a minute (the 32 KB leak's pool reading is retired)

MinimapOnceFields      g_fields;  // read and counted into by the installed code, through its address
platform::CodeRedirect g_circleCode;
platform::CodeRedirect g_octagonCode;
bool                   g_installed = false;
bool                   g_on        = false;

// Game thread.
std::uint64_t g_playerTwoPasses = 0;
std::uint64_t g_logged[5]       = {};  // the fields' four counters and the passes, at the last line
ULONGLONG     g_logAt           = 0;

std::uint64_t Counter(const std::uint64_t& field) noexcept
{
    return reinterpret_cast<const volatile std::uint64_t&>(field);
}

bool Install(const platform::GameImage& image, platform::CodeRedirect& redirect, const WidgetSite& widget,
             std::span<const std::uint8_t> code) noexcept
{
    const auto site = image.Rebase(widget.site, widget.siteBytes.size());
    if (!site || !image.BytesMatch(widget.acquireCall, widget.acquireBytes) ||
        !image.BytesMatch(widget.submitCall, widget.submitBytes) ||
        !image.BytesMatch(widget.afterSubmit, widget.afterSubmitBytes))
    {
        platform::Log(L"minimap once: the {}'s paint is not what the research read; its map renders in both passes",
                      widget.name);
        return false;
    }
    if (!redirect.Install(*site, widget.siteBytes, code, *site + widget.siteBytes.size()))
    {
        platform::Log(L"minimap once: the {}'s site could not be redirected; its map renders in both passes", widget.name);
        return false;
    }
    return true;
}

}  // namespace

bool InstallMinimapOnce(const platform::GameImage& image) noexcept
{
    // The installed code reads g_fields, which lives in this DLL: it must never unload while that code can run.
    if (!platform::PinThisModule())
    {
        platform::Log(L"minimap once: this DLL could not be pinned; the minimap renders in both passes");
        return false;
    }
    const auto circleAfter  = image.Rebase(kCircle.afterSubmit);
    const auto octagonAfter = image.Rebase(kOctagon.afterSubmit);
    const bool circle =
        circleAfter && (platform::RunningBuild() == platform::BuildIndex::V1005
                            ? Install(image, g_circleCode, kCircle1005, CircleSkipCode1005(g_fields, *circleAfter))
                            : Install(image, g_circleCode, kCircle, CircleSkipCode(g_fields, *circleAfter)));
    const bool octagon = octagonAfter && Install(image, g_octagonCode, kOctagon, OctagonSkipCode(g_fields, *octagonAfter));
    g_installed = circle || octagon;
    g_on        = g_installed;
    if (g_installed)
    {
        platform::Log(L"minimap once: installed (circle {}, octagon {}); player 2's HUD pass skips the minimap's own map "
                      L"render, player 1's pass renders it once per frame",
                      circle ? L"yes" : L"no", octagon ? L"yes" : L"no");
    }
    return g_installed;
}

void RemoveMinimapOnce() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on = false;
    bool all = true;
    for (platform::CodeRedirect* code : {&g_circleCode, &g_octagonCode})
    {
        all = (!code->IsInstalled() || code->Restore()) && all;
    }
    if (!all)
    {
        platform::Log(L"minimap once: a site was changed after us; left in place (it passes through with the switch off)");
    }
    g_installed = false;
}

bool MinimapOnceInstalled() noexcept
{
    return g_installed;
}

void SetMinimapOnce(bool on) noexcept
{
    g_on = on && g_installed;
    platform::Log(L"minimap once: {}", g_on ? L"on (player 1's pass alone renders the map)"
                                             : L"off (the map renders in both passes, as the game code runs)");
}

bool MinimapOnceOn() noexcept
{
    return g_on;
}

void BeginMinimapPlayerTwoPass(bool playerTwoMinimap) noexcept
{
    ++g_playerTwoPasses;
    reinterpret_cast<volatile std::uint64_t&>(g_fields.skip) = g_on && !playerTwoMinimap ? 1 : 0;
}

void EndMinimapPlayerTwoPass() noexcept
{
    reinterpret_cast<volatile std::uint64_t&>(g_fields.skip) = 0;
}

void PollMinimapOnce() noexcept
{
    if (!g_installed)
    {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_logAt)
    {
        return;
    }
    g_logAt = now + kLogPeriodMs;
    const std::uint64_t counts[5] = {Counter(g_fields.circlePainted), Counter(g_fields.circleSkipped),
                                     Counter(g_fields.octagonPainted), Counter(g_fields.octagonSkipped), g_playerTwoPasses};
    const bool painted = counts[0] != g_logged[0] || counts[1] != g_logged[1] || counts[2] != g_logged[2] ||
                         counts[3] != g_logged[3];
    if (!painted)
    {
        return;  // no minimap on screen this period (the open world shows a compass)
    }
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"minimap once ({}): in the last period the circle's map rendered {} and skipped {} times, the "
                           L"octagon's rendered {} and skipped {}; player 2's HUD passes {}",
                           g_on ? L"on" : L"off", counts[0] - g_logged[0], counts[1] - g_logged[1],
                           counts[2] - g_logged[2], counts[3] - g_logged[3], counts[4] - g_logged[4]);
    }
    for (int i = 0; i < 5; ++i)
    {
        g_logged[i] = counts[i];
    }
}

}  // namespace lss::hud
