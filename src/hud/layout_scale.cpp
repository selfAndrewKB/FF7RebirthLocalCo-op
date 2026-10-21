#include "hud/layout_scale.h"

#include "hud/layout_scale_code.h"
#include "platform/build_value.h"
#include "platform/code_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/image.h"
#include "platform/log.h"

#include <atomic>
#include <cstdint>
#include <span>

namespace lss::hud {

using namespace layout_scale_code;  // the site, its bytes and the code placed there

namespace {

platform::CodeRedirect  g_redirect;
platform::DeferredSites g_placement;  // 1.005: placed from the game thread, outside the overlay render
std::uintptr_t          g_site   = 0;  // the instruction replaced (1.005: the two before it)
std::uintptr_t          g_resume = 0;

/// The redirect: at install, or on 1.005 from the game thread between its ticks, where the two instructions before the
/// site are replaced instead (hud/layout_scale_code.h).
bool PlaceScale(const platform::GameImage&) noexcept
{
    const std::span<const std::uint8_t> context(kContextBytes);
    const bool ok = g_placement.FromGameThread()
                        ? g_redirect.Install(g_site, context.first(kReplaced1005), kCode1005, g_resume)
                        : g_redirect.Install(g_site, context.subspan(kSite - kContext, kReplaced), kCode, g_resume);
    platform::Log(L"HUD layout scale: {}",
                  !ok ? L"the overlay's layout-scale code could not be redirected; left alone (side-by-side halves "
                        L"keep the game's height rule)"
                  : g_placement.FromGameThread()
                      ? L"the overlay lays out by the contain rule (unchanged for 16:9; installed from the game thread)"
                      : L"the overlay lays out by the contain rule (unchanged for 16:9)");
    return ok;
}

}  // namespace

bool InstallLayoutScale(const platform::GameImage& image) noexcept
{
    const bool deferred = platform::DeferredSites::Deferred();
    const auto site     = image.Rebase(deferred ? kContext : kSite, deferred ? kReplaced1005 : kReplaced);
    const auto resume   = image.Rebase(kResume);
    if (!site || !resume || !image.BytesMatch(kContext, kContextBytes))
    {
        platform::Log(L"HUD layout scale: the overlay's layout-scale code is not what the research read; left alone "
                      L"(side-by-side halves keep the game's height rule)");
        return false;
    }
    g_site   = *site;
    g_resume = *resume;
    const bool placed = g_placement.Install(image, &PlaceScale, deferred);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"HUD layout scale: {}",
                      placed ? L"checked; the contain rule follows on the game thread"
                             : L"no room to place the contain rule from the game thread; left alone");
    }
    return placed;
}

void RemoveLayoutScale() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    if (g_redirect.IsInstalled() && !g_redirect.Restore())
    {
        platform::Log(L"HUD layout scale: could not restore the game's instruction (changed after us); left in place");
    }
}

}  // namespace lss::hud
