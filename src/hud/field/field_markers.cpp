#include "hud/field/field_markers.h"

#include "facts/hud.h"
#include "hud/passes/pass_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"
#include "scopes/leader_scope.h"
#include "world/field_pickup.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lss::hud::field_markers {

namespace {

// The marker list (research/RE-p2-field-interact.md): EndUISystem+0x98 points to a TSparseArray of 140-byte elements.
// The element's weak widgets: +0x00 the marker (projected; a point's marker is a point's +0x78), +0x08 made last at
// creation, +0x28 placed at the point, +0x30 animated. The game's destroy releases all four with the element.
constexpr std::uint32_t  kMarkerWidgets[] = {0x00, 0x08, 0x28, 0x30};

struct Hidden
{
    std::size_t markers = 0, widgets = 0;  // the other players' point markers hidden, and their widgets
    bool operator==(const Hidden&) const = default;
};

std::uintptr_t g_system = 0;  // set once at install
// Game thread.
HiddenSet      g_hidden;  // the other players' markers, transparent for a pass
std::array<Hidden, players::kMaxPlayers> g_logged = [] {
    std::array<Hidden, players::kMaxPlayers> logged{};
    for (Hidden& each : logged)
    {
        each.markers = static_cast<std::size_t>(-1);  // the first pass logs
    }
    return logged;
}();

void LogIfChanged(int seat, const Hidden& now) noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || now == g_logged[static_cast<std::size_t>(seat)])
    {
        return;
    }
    g_logged[static_cast<std::size_t>(seat)] = now;
    try
    {
        platform::LogDebug(L"field markers: {}'s view hides {} of the other players' point marker(s) ({} widget(s))",
                           players::SeatName(seat), now.markers, now.widgets);
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(facts::kEndUISystem, facts::kUIFieldMarkers + sizeof(void*));
    g_system          = system ? *system : 0;
    g_hidden.Reserve(256);  // four widgets per marker; grows (guarded) past that
    platform::Log(L"field markers: each view shows only its own player's point markers: {}",
                  g_system == 0 ? L"UNAVAILABLE (no marker list)"
                  : !players::widget_opacity::Installed()
                      ? L"UNAVAILABLE (the opacity setter is not what the research read)"
                      : L"ready");
}

void ShowOnly(int seat) noexcept
{
    Restore();
    std::uintptr_t list = 0;
    if (seat < 0 || seat >= players::kMaxPlayers || g_system == 0 || !players::widget_opacity::Installed() ||
        !platform::CopyGuarded(&list, reinterpret_cast<const void*>(g_system + facts::kUIFieldMarkers), sizeof(list)) ||
        list == 0)
    {
        return;
    }
    Hidden hidden;
    try
    {
        platform::ForEachSparseElement(list, facts::kFieldMarkerEntry, [seat, &hidden](std::uintptr_t entry) {
            const int owner = world::field_pickup::MarkerOwner(
                players::weak_object::ObjectAt(reinterpret_cast<const void*>(entry)));
            if (owner < 0 || owner == seat)
            {
                return true;  // this view's own, or not a point's marker
            }
            ++hidden.markers;
            for (const std::uint32_t at : kMarkerWidgets)
            {
                std::uint64_t weak   = 0;
                void*         widget = nullptr;
                if (!platform::CopyGuarded(&weak, reinterpret_cast<const void*>(entry + at), sizeof(weak)) || weak == 0 ||
                    (widget = players::weak_object::ObjectAt(reinterpret_cast<const void*>(entry + at))) == nullptr)
                {
                    continue;
                }
                if (g_hidden.Hide(widget, weak))
                {
                    ++hidden.widgets;
                }
            }
            return true;
        });
    }
    catch (...)
    {
        // What was hidden before the throw is listed and comes back in Restore.
    }
    LogIfChanged(seat, hidden);
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::hud::field_markers
