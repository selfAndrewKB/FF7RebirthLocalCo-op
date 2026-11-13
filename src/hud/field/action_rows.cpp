#include "hud/field/action_rows.h"

#include "facts/hud.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/weak_object.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::action_rows {

namespace {

constexpr std::uint32_t kListNum = 0x8;  // the list's num, after its data pointer

using SortAndLayoutFn = void (*)(void* list, void* placeNow);

// Set once at install.
std::uintptr_t  g_endUISystem   = 0;
SortAndLayoutFn g_sortAndLayout = nullptr;

using platform::Read;  // the guarded reads: platform/guarded.h

bool LayoutGuarded(void* list, void* placeNow) noexcept
{
    __try
    {
        g_sortAndLayout(list, placeNow);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_endUISystem != 0)
    {
        return true;
    }
    const auto system = players::game_globals::Address(players::game_globals::Global::UiSystem);
    const auto layout = platform::FindFunction(image, facts::kSortAndLayout);
    g_endUISystem     = system;
    g_sortAndLayout   = layout ? reinterpret_cast<SortAndLayoutFn>(*layout) : nullptr;
    return g_endUISystem != 0;
}

bool ListKnown() noexcept
{
    return g_endUISystem != 0;
}

bool LayoutAvailable() noexcept
{
    return g_sortAndLayout != nullptr;
}

std::uintptr_t List() noexcept
{
    std::uintptr_t list = 0;
    return g_endUISystem != 0 && Read(g_endUISystem + facts::kUIActionRows, list) ? list : 0;
}

bool Rows(std::vector<RowEntry>& rows)
{
    rows.clear();
    const std::uintptr_t list = List();
    std::uintptr_t       data = 0;
    std::int32_t         num  = 0;
    if (list == 0 || !Read(list, data) || !Read(list + kListNum, num) || num < 0 || num > kMaxRows)
    {
        return false;
    }
    for (std::int32_t i = 0; i < num; ++i)
    {
        RowEntry row;
        if (!Read(data + sizeof(std::uint64_t) * static_cast<std::size_t>(i), row.weak) ||
            (row.widget = players::weak_object::ObjectOf(row.weak)) == nullptr)
        {
            continue;
        }
        rows.push_back(row);
    }
    return true;
}

LayoutPoint PositionOf(const void* widget) noexcept
{
    LayoutPoint position;
    Read(reinterpret_cast<std::uintptr_t>(widget) + facts::kUserWidgetViewportOffsets, position);
    return position;
}

bool LayOut(void* list, std::span<void* const> rows, bool again, float width, float height, void* placeNow) noexcept
{
    if (g_sortAndLayout == nullptr || list == nullptr)
    {
        return g_sortAndLayout != nullptr;
    }
    // Each call places the row it names at once and gives the others their slide target (the sort and the slots come
    // out the same each time). After a base change, each row stranded outside the layout is named in a call of its
    // own; the others slide.
    bool named = false;
    bool ok    = true;
    for (void* row : again ? rows : std::span<void* const>{})
    {
        if (OutsideLayout(PositionOf(row), width, height))
        {
            named = true;
            ok    = ok && LayoutGuarded(list, row);
        }
    }
    if (!named)
    {
        ok = LayoutGuarded(list, placeNow);
    }
    if (!ok)
    {
        g_sortAndLayout = nullptr;  // never again this session
    }
    return ok;
}

}  // namespace lss::hud::action_rows
