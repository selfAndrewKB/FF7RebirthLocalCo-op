#include "hud/field/wheelie_tops.h"

#include "hud/passes/pass_opacity.h"
#include "platform/log.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/widget_opacity.h"
#include "rides/wheelie_ride.h"

#include <array>
#include <cstddef>

namespace lss::hud::wheelie_tops {

namespace {

// Game thread.
HiddenSet                                     g_hidden;
std::array<std::size_t, players::kMaxPlayers> g_hiddenLogged{};
std::array<bool, players::kMaxPlayers>        g_loggedOnce{};

void LogIfChanged(int seat, std::size_t hidden) noexcept
{
    const auto index = static_cast<std::size_t>(seat);
    if (!platform::Enabled(platform::Level::Debug) || (g_loggedOnce[index] && hidden == g_hiddenLogged[index]))
    {
        return;
    }
    g_hiddenLogged[index] = hidden;
    g_loggedOnce[index]   = true;
    platform::LogDebug(L"wheelie counters: {}'s view: {} other player's counter(s) hidden", players::SeatName(seat), hidden);
}

}  // namespace

void ShowOnly(int seat) noexcept
{
    Restore();
    if (seat < 0 || seat >= players::kMaxPlayers || !players::widget_opacity::Installed() ||
        !players::generation::OnGameThread())
    {
        return;
    }
    const rides::wheelie_ride::TopWidgets tops = rides::wheelie_ride::TopWidgetsNow();
    std::size_t                           hidden = 0;
    for (int player = 0; player < players::kMaxPlayers; ++player)
    {
        if (player != seat && g_hidden.Hide(tops.widget[player]))
        {
            ++hidden;
        }
    }
    LogIfChanged(seat, hidden);
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::hud::wheelie_tops
