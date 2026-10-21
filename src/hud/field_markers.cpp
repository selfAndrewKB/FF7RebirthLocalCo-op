#include "hud/field_markers.h"

#include "facts/hud.h"
#include "hud/widget_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "player_two/field_pickup.h"
#include "player_two/leader_scope.h"
#include "player_two/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lss::hud::field_markers {

namespace {

// The marker list (RE-p2-field-interact.md §12.14): EndUISystem+0x98 points to a TSparseArray of 140-byte elements.
// The element's weak widgets: +0x00 the marker (projected; a point's marker is a point's +0x78), +0x08 made last at
// creation, +0x28 placed at the point, +0x30 animated. The game's destroy releases all four with the element (§12.15).
constexpr std::uint32_t  kMarkerWidgets[] = {0x00, 0x08, 0x28, 0x30};

struct Hidden
{
    std::uint64_t weak     = 0;
    float         previous = 1.0f;  // the widget's own render opacity
};

struct Half
{
    std::size_t markers = 0, widgets = 0;  // the other player's point markers hidden, and their widgets
    bool operator==(const Half&) const = default;
};

std::uintptr_t      g_system = 0;  // set once at install
// Game thread.
std::vector<Hidden> g_hidden;
std::array<Half, 2> g_logged{{{static_cast<std::size_t>(-1), 0}, {static_cast<std::size_t>(-1), 0}}};

void LogIfChanged(int player, const Half& now) noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || now == g_logged[player])
    {
        return;
    }
    g_logged[player] = now;
    try
    {
        platform::LogDebug(L"field markers: player {}'s half hides {} of player {}'s point marker(s) ({} widget(s))", player + 1,
                      now.markers, 2 - player, now.widgets);
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
    try
    {
        g_hidden.reserve(256);  // four widgets per marker; grows (guarded) past that
    }
    catch (...)
    {
    }
    platform::Log(L"field markers: each half shows only its own player's point markers: {}",
                  g_system == 0                    ? L"UNAVAILABLE (no marker list)"
                  : !widget_opacity::Installed() ? L"UNAVAILABLE (the opacity setter is not what the research read)"
                                                   : L"ready");
}

void ShowOnly(int player) noexcept
{
    Restore();
    std::uintptr_t list = 0;
    if (g_system == 0 || !widget_opacity::Installed() ||
        !platform::CopyGuarded(&list, reinterpret_cast<const void*>(g_system + facts::kUIFieldMarkers), sizeof(list)) ||
        list == 0)
    {
        return;
    }
    const int mine = player != 0 ? 1 : 0;
    Half      half;
    try
    {
        platform::ForEachSparseElement(list, facts::kFieldMarkerEntry, [mine, &half](std::uintptr_t entry) {
            const int owner = player_two::field_pickup::MarkerOwner(
                player_two::weak_object::ObjectAt(reinterpret_cast<const void*>(entry)));
            if (owner < 0 || owner == mine)
            {
                return true;  // this half's own, or not a point's marker
            }
            ++half.markers;
            for (const std::uint32_t at : kMarkerWidgets)
            {
                std::uint64_t weak   = 0;
                void*         widget = nullptr;
                if (!platform::CopyGuarded(&weak, reinterpret_cast<const void*>(entry + at), sizeof(weak)) || weak == 0 ||
                    (widget = player_two::weak_object::ObjectAt(reinterpret_cast<const void*>(entry + at))) == nullptr)
                {
                    continue;
                }
                const float previous = widget_opacity::Get(widget);
                if (previous != 0.0f && widget_opacity::Set(widget, 0.0f))
                {
                    g_hidden.push_back({weak, previous});
                    ++half.widgets;
                }
            }
            return true;
        });
    }
    catch (...)
    {
        // What was hidden before the throw is listed and comes back in Restore.
    }
    LogIfChanged(mine, half);
}

void Restore() noexcept
{
    for (const Hidden& hidden : g_hidden)
    {
        if (void* widget = player_two::weak_object::ObjectAt(&hidden.weak))
        {
            widget_opacity::Set(widget, hidden.previous);
        }
    }
    g_hidden.clear();
}

}  // namespace lss::hud::field_markers
