#pragma once

// The pure rules of the engine's layout records for the view columns. Host-tested (tests/column_rules_test.cpp).
//
// The engine lays its local players out every redraw from its own layout table (UGameViewportClient::SplitscreenInfo,
// filled once by the viewport client's constructor; research/RE-splitscreen-layout-table.md): one entry per layout,
// each a list of records {SizeX, SizeY, OriginX, OriginY}, fractions of the window, and the view at place i of the
// local-player list takes record i. Three local players take the three-column entry the mod holds the three-player
// setting at (views/columns.h); two take the side-by-side or the top-and-bottom entry (players/split_layout.h). The
// stock records divide the window equally. On a span of monitors of different widths (views/window_span.h) the mod
// writes the entry's records to the monitors' widths, so each view fills its own monitor edge to edge, a wider monitor
// a wider view, and puts the stock records back after (views/column_layout.cpp). The records here are the ones compared
// and written; the writing itself is a data write the engine reads on its next redraw, not a recreation of the layout.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lss::views::column_rules {

/// One record of the engine's per-player layout data (FPerPlayerSplitscreenData): fractions of the viewport.
struct Record
{
    float sizeX = 0, sizeY = 0, originX = 0, originY = 0;
    bool  operator==(const Record&) const noexcept = default;
};

/// The engine's three-player layouts hold three records, its two-player layouts two.
inline constexpr int kThreePlayerRecords = 3;
inline constexpr int kTwoPlayerRecords   = 2;

/// The stock three-column entry (ThreePlayer_Vertical), as the viewport client's constructor fills it [CONFIRMED
/// static, RE-splitscreen-layout-table.md]: three equal full-height columns, left to right.
inline constexpr Record kStockThreeColumns[kThreePlayerRecords] = {
    {1.0f / 3.0f, 1.0f, 0.0f, 0.0f},
    {1.0f / 3.0f, 1.0f, 1.0f / 3.0f, 0.0f},
    {1.0f / 3.0f, 1.0f, 2.0f / 3.0f, 0.0f},
};

/// The stock two-player entries, as the constructor fills them: TwoPlayer_Horizontal (player 1 on top, the other
/// below) and TwoPlayer_Vertical (player 1 on the left, the other on the right).
inline constexpr Record kStockTopBottom[kTwoPlayerRecords] = {
    {1.0f, 0.5f, 0.0f, 0.0f},
    {1.0f, 0.5f, 0.0f, 0.5f},
};
inline constexpr Record kStockSideBySide[kTwoPlayerRecords] = {
    {0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, 1.0f, 0.5f, 0.0f},
};

template <std::size_t N>
constexpr bool Same(const Record (&a)[N], const Record (&b)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!(a[i] == b[i]))
        {
            return false;
        }
    }
    return true;
}

/// An entry's records: place i's record is record[i].
template <std::size_t N>
struct RecordSet
{
    Record record[N]{};
};
using Records    = RecordSet<kThreePlayerRecords>;
using TwoRecords = RecordSet<kTwoPlayerRecords>;

/// `pixels` of `total` as the fraction the engine turns back into exactly `pixels`. The engine places a view at
/// TruncToInt(fraction * viewport size), the product in float (ULocalPlayer::GetProjectionData [CONFIRMED both
/// builds]), and the nearest float to 2560/8560 lands a hair below 2560: the view would start a pixel short of its
/// monitor's edge. So the fraction is stepped up to the first float whose product truncates to `pixels` (0 for bad
/// input).
inline float EdgeFraction(std::int32_t pixels, std::int32_t total) noexcept
{
    if (pixels <= 0 || total <= 0 || pixels > total)
    {
        return 0.0f;
    }
    const float size     = static_cast<float>(total);
    float       fraction = static_cast<float>(pixels) / size;
    for (int step = 0; step < 4 && std::trunc(fraction * size) < static_cast<float>(pixels); ++step)
    {
        fraction = std::nextafter(fraction, 2.0f);
    }
    return fraction;
}

/// Full-height columns for a row of monitors of these widths, left to right (the span's, views/window_span): each view
/// fills its own monitor edge to edge, a wider monitor a wider view, as the game fits a single view to the width of the
/// monitor it is on. Equal widths give equal columns. Any width not above 0: every record empty.
template <std::size_t N>
RecordSet<N> ColumnRecords(const std::array<std::int32_t, N>& widths) noexcept
{
    RecordSet<N> out{};
    std::int32_t total = 0;
    for (const std::int32_t width : widths)
    {
        if (width <= 0)
        {
            return out;
        }
        total += width;
    }
    std::int32_t left = 0;
    for (std::size_t i = 0; i < N; ++i)
    {
        out.record[i] = {EdgeFraction(widths[i], total), 1.0f, left == 0 ? 0.0f : EdgeFraction(left, total), 0.0f};
        left += widths[i];
    }
    return out;
}

/// The same with each view as tall as its monitor, its top at the window's top: a monitor shorter than the row's
/// tallest shows the top part of the window, where its view is.
template <std::size_t N>
RecordSet<N> ColumnRecords(const std::array<std::int32_t, N>& widths, const std::array<std::int32_t, N>& heights) noexcept
{
    RecordSet<N> out     = ColumnRecords(widths);
    std::int32_t tallest = 0;
    for (const std::int32_t height : heights)
    {
        tallest = std::max(tallest, height);
    }
    for (std::size_t i = 0; i < N; ++i)
    {
        if (heights[i] <= 0)
        {
            return ColumnRecords(widths);
        }
        out.record[i].sizeY = heights[i] == tallest ? 1.0f : EdgeFraction(heights[i], tallest);
    }
    return out;
}

/// The entry holds exactly `base`'s records, in any order (each once).
template <std::size_t N>
constexpr bool ArrangementOf(const Record (&entry)[N], const Record (&base)[N]) noexcept
{
    std::array<bool, N> used{};
    for (std::size_t i = 0; i < N; ++i)
    {
        bool found = false;
        for (std::size_t j = 0; !found && j < N; ++j)
        {
            if (!used[j] && entry[i] == base[j])
            {
                used[j] = true;
                found   = true;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

/// What the module does with the entry it read, given the entry wanted (the stock one, or the mod's columns).
enum class Action
{
    Nothing,  // the entry is the one wanted
    Write,    // the mod's entry wanted, and the entry is the stock one or another of the mod's: write it
    Restore,  // the stock entry wanted, and the entry holds one of the mod's: put the stock records back
    Foreign,  // the entry is neither the stock one nor one of the mod's: left alone (logged once)
};

/// The three-column entry: `columns` the columns in effect (the span's monitors' widths, or the stock thirds);
/// `written` the entry the mod wrote last (the stock one before any write): the columns follow the span's monitors, so
/// the entry may still hold the records of an earlier row.
constexpr Action Decide(const Record (&entry)[kThreePlayerRecords], const Record (&wanted)[kThreePlayerRecords],
                        const Record (&columns)[kThreePlayerRecords], const Record (&written)[kThreePlayerRecords]) noexcept
{
    if (!ArrangementOf(entry, kStockThreeColumns) && !ArrangementOf(entry, columns) && !Same(entry, written))
    {
        return Action::Foreign;
    }
    if (Same(entry, wanted))
    {
        return Action::Nothing;
    }
    return Same(wanted, kStockThreeColumns) ? Action::Restore : Action::Write;
}

/// A two-player entry: as Decide, with its own stock records, the base in effect (`base`: the two monitors' widths on
/// a two-monitor span, else the stock ones) and the entry last written (`written`) as the known contents.
constexpr Action DecideTwo(const Record (&entry)[kTwoPlayerRecords], const Record (&wanted)[kTwoPlayerRecords],
                           const Record (&stock)[kTwoPlayerRecords], const Record (&base)[kTwoPlayerRecords],
                           const Record (&written)[kTwoPlayerRecords]) noexcept
{
    if (!ArrangementOf(entry, stock) && !ArrangementOf(entry, base) && !Same(entry, written))
    {
        return Action::Foreign;
    }
    if (Same(entry, wanted))
    {
        return Action::Nothing;
    }
    return Same(wanted, stock) ? Action::Restore : Action::Write;
}

/// The three-column entry wanted: the columns in effect while three local players are listed (the engine lays three
/// out from this entry and from no other), the stock records otherwise (nothing reads the entry then, and the span's
/// records must not outlive the span).
constexpr Records ThreeWanted(const Records& columns, int localPlayers) noexcept
{
    return localPlayers >= kThreePlayerRecords ? columns : Records{{kStockThreeColumns[0], kStockThreeColumns[1], kStockThreeColumns[2]}};
}

/// A two-player entry wanted: its base while exactly two local players are listed, its stock records otherwise.
constexpr TwoRecords TwoWanted(const TwoRecords& base, const Record (&stock)[kTwoPlayerRecords], int localPlayers) noexcept
{
    return localPlayers == kTwoPlayerRecords ? base : TwoRecords{{stock[0], stock[1]}};
}

}  // namespace lss::views::column_rules
