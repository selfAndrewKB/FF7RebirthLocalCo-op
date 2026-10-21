#include "hud/layout_scale.h"

#include "hud/layout_scale_code.h"
#include "platform/build_value.h"
#include "platform/code_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <atomic>
#include <cstdint>
#include <span>

namespace lss::hud {

using namespace layout_scale_code;  // the site, its bytes and the code placed there

namespace {

platform::CodeRedirect g_redirect;
std::atomic<bool>       g_pending      {false};  // 1.005: checked, waiting for the game thread
std::uintptr_t         g_pendingSite   = 0;
std::uintptr_t         g_pendingResume = 0;

}  // namespace

bool InstallLayoutScale(const platform::GameImage& image) noexcept
{
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        const auto site   = image.Rebase(kContext, kReplaced1005);
        const auto resume = image.Rebase(kResume);
        if (!site || !resume || !image.BytesMatch(kContext, kContextBytes))
        {
            platform::Log(L"HUD layout scale: the overlay's layout-scale code is not what the research read; left alone "
                          L"(side-by-side halves keep the game's height rule)");
            return false;
        }
        g_pendingSite   = *site;
        g_pendingResume = *resume;
        g_pending       = true;
        platform::Log(L"HUD layout scale: checked; the contain rule follows on the game thread");
        return true;
    }
    const auto site   = image.Rebase(kSite, kReplaced);
    const auto resume = image.Rebase(kResume);
    if (!site || !resume || !image.BytesMatch(kContext, kContextBytes) ||
        !g_redirect.Install(*site, std::span<const std::uint8_t>(kContextBytes + (kSite - kContext), kReplaced), kCode,
                            *resume))
    {
        platform::Log(L"HUD layout scale: the overlay's layout-scale code is not what the research read; left alone "
                      L"(side-by-side halves keep the game's height rule)");
        return false;
    }
    platform::Log(L"HUD layout scale: the overlay lays out by the contain rule (unchanged for 16:9)");
    return true;
}

void FinishLayoutScale() noexcept
{
    if (!g_pending || !player_two::generation::OnGameThread())
    {
        return;
    }
    g_pending = false;
    const bool ok = g_redirect.Install(g_pendingSite, std::span<const std::uint8_t>(kContextBytes, kReplaced1005), kCode1005,
                                       g_pendingResume);
    platform::Log(L"HUD layout scale: {}", ok ? L"the overlay lays out by the contain rule (unchanged for 16:9; "
                                                L"installed from the game thread)"
                                              : L"the overlay's layout-scale code could not be redirected; left alone "
                                                L"(side-by-side halves keep the game's height rule)");
}

void RemoveLayoutScale() noexcept
{
    g_pending = false;
    if (g_redirect.IsInstalled() && !g_redirect.Restore())
    {
        platform::Log(L"HUD layout scale: could not restore the game's instruction (changed after us); left in place");
    }
}

}  // namespace lss::hud
