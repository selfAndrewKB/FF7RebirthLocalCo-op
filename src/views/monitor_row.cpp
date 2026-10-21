#include "views/monitor_row.h"

#include <algorithm>
#include <format>
#include <vector>

namespace lss::views::monitor_row {

namespace {

/// Why `candidate` cannot stand beside `base` in the row, or empty if it can.
std::wstring Mismatch(const Monitor& base, const Monitor& candidate)
{
    if (candidate.dpi != 96)
    {
        return std::format(L"scaled to {} % in Windows (needs 100 %)", candidate.dpi * 100 / 96);
    }
    if (candidate.rect.Width() != base.rect.Width() || candidate.rect.Height() != base.rect.Height())
    {
        return std::format(L"{}x{}, not {}x{}", candidate.rect.Width(), candidate.rect.Height(), base.rect.Width(),
                           base.rect.Height());
    }
    if (candidate.rect.top != base.rect.top)
    {
        return std::format(L"its top edge is {} px off the game's monitor's", candidate.rect.top - base.rect.top);
    }
    return {};
}

/// The monitor touching `edge` (its right edge there when `leftOf`, else its left edge) with any vertical overlap.
const Monitor* Neighbour(std::span<const Monitor> monitors, const Rect& edge, bool leftOf) noexcept
{
    for (const auto& m : monitors)
    {
        const bool touches = leftOf ? m.rect.right == edge.left : m.rect.left == edge.right;
        const bool overlap = m.rect.top < edge.bottom && m.rect.bottom > edge.top;
        if (touches && overlap)
        {
            return &m;
        }
    }
    return nullptr;
}

}  // namespace

Choice Choose(std::span<const Monitor> monitors, std::size_t gameMonitor, std::int32_t columns)
{
    Choice choice;
    if (gameMonitor >= monitors.size())
    {
        choice.why = L"the game's monitor is not in the list";
        return choice;
    }
    if (columns < 1)
    {
        choice.why = L"no columns asked for";
        return choice;
    }
    const Monitor& base = monitors[gameMonitor];
    if (base.dpi != 96)
    {
        choice.why = std::format(L"the game's monitor is scaled to {} % in Windows (needs 100 %)", base.dpi * 100 / 96);
        return choice;
    }

    // The row: equal monitors touching the game's, outward to each side until one is missing or unequal.
    std::vector<Rect> row{base.rect};
    std::wstring      stoppedBy;
    for (const bool leftOf : {true, false})
    {
        Rect edge = base.rect;
        for (;;)
        {
            const Monitor* next = Neighbour(monitors, edge, leftOf);
            if (next == nullptr)
            {
                break;
            }
            const auto mismatch = Mismatch(base, *next);
            if (!mismatch.empty())
            {
                if (stoppedBy.empty())
                {
                    stoppedBy = std::format(L"the monitor at ({}, {}) is {}", next->rect.left, next->rect.top, mismatch);
                }
                break;
            }
            row.push_back(next->rect);
            edge = next->rect;
        }
    }
    std::sort(row.begin(), row.end(), [](const Rect& a, const Rect& b) { return a.left < b.left; });
    choice.rowMonitors = static_cast<std::int32_t>(row.size());
    if (choice.rowMonitors < columns)
    {
        choice.why = std::format(L"{} equal monitor(s) in the game's row, {} needed{}{}", choice.rowMonitors, columns,
                                 stoppedBy.empty() ? L"" : L"; ", stoppedBy);
        return choice;
    }

    const auto gameAt = static_cast<std::int32_t>(
        std::find(row.begin(), row.end(), base.rect) - row.begin());
    const auto start = std::min(gameAt, choice.rowMonitors - columns);
    choice.ok        = true;
    choice.span      = {row[static_cast<std::size_t>(start)].left, base.rect.top,
                        row[static_cast<std::size_t>(start + columns - 1)].right, base.rect.bottom};
    return choice;
}

}  // namespace lss::views::monitor_row
