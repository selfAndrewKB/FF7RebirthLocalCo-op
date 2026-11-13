#include "views/column_layout.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "players/game_globals.h"
#include "players/held_setting.h"
#include "players/split_settings_access.h"
#include "views/column_rules.h"

#include "platform/command_registry.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace lss::views::column_layout {

namespace {

using column_rules::Action;
using column_rules::kStockThreeColumns;
using column_rules::kThreePlayerRecords;
using column_rules::kTwoPlayerRecords;
using column_rules::ListSeats;
using column_rules::Order;
using column_rules::Record;
using column_rules::Records;
using column_rules::TwoRecords;

static_assert(sizeof(Record) == facts::kPerPlayerSplitBytes, "a record is the engine's FPerPlayerSplitscreenData");

constexpr std::uint32_t  kCheckTicks         = 30;   // an entry re-read twice a second at 60 while written, foreign, faulted or wanted
constexpr std::uint32_t  kStatusTicks        = 300;
constexpr std::uint8_t   kThreeColumnsLayout = 2;    // EThreePlayerSplitScreenType::Vertical: the entry written
constexpr const wchar_t* kThreeLayoutName    = L"ThreePlayerSplitscreenLayout";

// The three-player layout, held at three columns while three local players are listed (players/held_setting.h).
std::optional<std::uint8_t> ReadThreePlayerLayout(const wchar_t*) noexcept
{
    return players::split_settings::ThreePlayerLayout();
}

bool WriteThreePlayerLayout(const wchar_t*, std::uint8_t value) noexcept
{
    return players::split_settings::SetThreePlayerLayout(value);
}

constexpr Records StockThree() noexcept
{
    return Records{{kStockThreeColumns[0], kStockThreeColumns[1], kStockThreeColumns[2]}};
}

// Set at install, then only read.
std::uintptr_t g_engineGlobal = 0;
bool           g_installed    = false;

// Game thread.
players::HeldSetting<std::uint8_t> g_threeLayout{{&ReadThreePlayerLayout, &WriteThreePlayerLayout}, kThreeLayoutName};
int            g_localPlayers = 0;        // the engine's local players at the last tick
bool           g_written      = false;    // the three-column entry holds the mod's records
Records        g_wanted       = StockThree();  // the entry the last Apply wanted
Records        g_columnsBase  = StockThree();  // the columns in effect: the span's monitors' widths, else the stock thirds
Records        g_lastWrite    = StockThree();  // the entry the mod wrote last (the stock one before any write, after a restore)
Order          g_order        = column_rules::kDefaultOrder;  // which screen each player takes (the setting, the console)
column_rules::ThreeSplit g_threeSplit = column_rules::ThreeSplit::Top;  // three players in one window (the setting)
Records        g_oneWindow    = column_rules::ThreeSplitRecords(column_rules::ThreeSplit::Top);
int            g_views        = 0;        // the views laid out at the last tick (the local players listed)
ListSeats      g_seats{0, -1, -1};        // the engine's list at the last tick: each place's seat, -1 for none
bool           g_spanned      = false;    // columns held at the last tick (the span's, or the console's equal thirds)
std::array<std::int32_t, kThreePlayerRecords> g_widths{};  // the span's three monitors' widths, 0 when not three
bool           g_reached      = false;    // the table was read once (the first reconcile is done)
bool           g_foreign      = false;    // the entry holds neither the stock records nor the mod's (logged once)
const wchar_t* g_waiting      = nullptr;  // why the table cannot be reached, logged once per reason
const wchar_t* g_fault        = nullptr;  // a read or write that failed, logged once until it succeeds
std::uint32_t  g_ticks = 0, g_writes = 0, g_restores = 0;

/// One of the engine's two-player entries, kept as the three-column one is: its stock records, or on a two-monitor
/// span the side-by-side entry's two monitors' widths while two local players are listed; any other content left
/// alone (logged once).
struct TwoEntry
{
    std::uint32_t  type;                       // its place in the table
    const Record (*stock)[kTwoPlayerRecords];  // the constructor's two records
    const wchar_t* name;                       // "top and bottom", "side by side"
    TwoRecords     base{};                     // the records in effect: the stock ones, or the two monitors' widths
    TwoRecords     lastWrite{};                // the entry the mod wrote last (the stock one before any write)
    TwoRecords     wanted{};
    std::array<std::int32_t, kTwoPlayerRecords> widths{};  // the two monitors' widths, 0 when not two
    bool           written = false;
    bool           foreign = false;
    bool           reached = false;
    const wchar_t* waiting = nullptr;          // why the entry cannot be reached, logged once per reason
    const wchar_t* fault   = nullptr;          // logged once until it succeeds
    std::uint32_t  writes = 0, restores = 0;
};
std::array<TwoEntry, 2> g_two{
    TwoEntry{facts::kSplitTypeTwoPlayerHorizontal, &column_rules::kStockTopBottom, L"top and bottom"},
    TwoEntry{facts::kSplitTypeTwoPlayerVertical, &column_rules::kStockSideBySide, L"side by side"},
};

/// A failed read or write, logged once per reason (state-change logging: the retry is the cadence's).
void Fault(const wchar_t* why) noexcept
{
    if (g_fault != why)
    {
        g_fault = why;
        platform::LogWarning(L"column layout: {}", why);
    }
}

/// A log line whose arguments are built with std::format: a failed allocation drops the line, never the frame.
template <class Line>
void Quietly(Line&& line) noexcept
{
    try
    {
        line();
    }
    catch (...)
    {
    }
}

/// The table cannot be reached (`why`, one literal per reason): logged once per reason and per entry.
void Waiting(const wchar_t*& waiting, const wchar_t* why) noexcept
{
    if (waiting != why)
    {
        waiting = why;
        platform::LogDebug(L"column layout: the layout table cannot be reached: {}", why);
    }
}

/// The records of entry `type` (`expected` of them) in the live viewport client, or null with the reason. The table
/// is the constructor's: ten entries, the two-player ones of two records, the three-player ones of three.
std::uint8_t* FindRecords(std::uint32_t type, std::int32_t expected, const wchar_t** why) noexcept
{
    const void*    engine  = platform::ReadPointerAt(g_engineGlobal);
    const void*    client  = players::game_globals::GameViewportClient();
    std::uintptr_t entries = 0, records = 0;
    std::int32_t   count   = 0;
    if (engine == nullptr)
    {
        *why = L"GEngine is not set";
        return nullptr;
    }
    if (client == nullptr)
    {
        *why = L"the engine has no game viewport client yet";
        return nullptr;
    }
    if (!platform::Read(client, facts::kViewportClientSplitscreenInfo, entries) ||
        !platform::Read(client, facts::kViewportClientSplitscreenInfo + facts::kTArrayNum, count) || entries == 0 ||
        count != facts::kSplitscreenTypes)
    {
        *why = L"the viewport client's layout table is not the constructor's ten entries";
        return nullptr;
    }
    const std::uintptr_t entry = entries + type * facts::kSplitscreenDataBytes;
    if (!platform::Read(entry, records) || !platform::Read(entry + facts::kTArrayNum, count) || records == 0 || count != expected)
    {
        *why = expected == kThreePlayerRecords ? L"the three-column entry does not hold three records"
                                               : L"a two-player entry does not hold two records";
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t*>(records);
}

std::uint8_t* FindRecords(const wchar_t** why) noexcept
{
    return FindRecords(facts::kSplitTypeThreePlayerVertical, kThreePlayerRecords, why);
}

template <std::size_t N>
std::wstring RecordsText(const Record (&records)[N])
{
    std::wstring text;
    for (const Record& r : records)
    {
        text += std::format(L"{}({:.3f}, {:.3f} at {:.3f}, {:.3f})", text.empty() ? L"" : L" ", r.sizeX, r.sizeY, r.originX, r.originY);
    }
    return text;
}

/// The columns as the log names them: "three equal columns", or with the span's widths in pixels.
std::wstring ColumnsText()
{
    if (!g_spanned)
    {
        return std::format(L"three players in one window ({})", column_rules::ThreeSplitWord(g_threeSplit));
    }
    if (g_widths[0] == 0 || (g_widths[0] == g_widths[1] && g_widths[1] == g_widths[2]))
    {
        return L"three equal columns";
    }
    return std::format(L"three columns, each its monitor's width ({} | {} | {} px)", g_widths[0], g_widths[1], g_widths[2]);
}

/// The first reach of the table: what it holds, for the log (the research's live check).
void NoteReached(const Record (&entry)[kThreePlayerRecords]) noexcept
{
    if (g_reached)
    {
        return;
    }
    g_reached = true;
    Quietly([&] {
        platform::Log(L"column layout: the engine's layout table reached ({} entries); the three-column entry holds {}",
                      facts::kSplitscreenTypes,
                      column_rules::Same(entry, kStockThreeColumns) ? std::wstring(L"the stock thirds") : RecordsText(entry));
    });
}

/// The three-column entry as it should be: `wanted` written, the stock one restored, or left alone (logged).
void Apply(const Records& wanted) noexcept
{
    g_wanted               = wanted;
    const bool     ours    = !column_rules::Same(wanted.record, kStockThreeColumns);
    const wchar_t* why     = L"";
    std::uint8_t*  records = FindRecords(&why);
    if (records == nullptr)
    {
        Waiting(g_waiting, why);
        return;
    }
    g_waiting = nullptr;
    Record entry[kThreePlayerRecords]{};
    if (!platform::CopyGuarded(entry, records, sizeof(entry)))
    {
        Fault(L"the engine's three-column layout could not be read; tried again twice a second");
        return;
    }
    NoteReached(entry);
    const Action action = column_rules::Decide(entry, wanted.record, g_columnsBase.record, g_oneWindow.record, g_lastWrite.record);
    if (action != Action::Foreign && g_foreign)
    {
        g_foreign = false;
        platform::Log(L"column layout: the engine's three-column layout is {} again",
                      column_rules::Same(entry, kStockThreeColumns) ? L"the stock one" : L"one of the mod's");
    }
    switch (action)
    {
        case Action::Write:
            if (platform::WriteGuarded(records, wanted.record, sizeof(wanted.record)))
            {
                g_written   = true;
                g_lastWrite = wanted;
                g_fault     = nullptr;
                ++g_writes;
                Quietly([&] {
                    platform::Log(L"column layout: the engine's three-player layout now shows {} in the screen order {}{}{} (was "
                                  L"{}); the engine lays three local players out from it on the next redraw (write {})",
                                  ColumnsText(), g_order.player[0], g_order.player[1], g_order.player[2],
                                  column_rules::Same(entry, kStockThreeColumns) ? L"the stock one" : L"another of the mod's", g_writes);
                });
            }
            else
            {
                Fault(L"the engine's three-column layout could not be written; the columns keep the engine's equal widths; "
                      L"tried again twice a second");
            }
            break;
        case Action::Restore:
            if (platform::WriteGuarded(records, kStockThreeColumns, sizeof(kStockThreeColumns)))
            {
                g_written   = false;
                g_lastWrite = StockThree();
                g_fault     = nullptr;
                ++g_restores;
                platform::Log(L"column layout: the engine's three-player layout put back to its stock thirds (restore {})", g_restores);
            }
            else
            {
                Fault(L"the engine's three-column layout could not be put back; it keeps the mod's records; tried again twice a second");
            }
            break;
        case Action::Foreign:
            g_fault = nullptr;  // the entry was read: a read or write fault is over, the foreign content is the state now
            if (!g_foreign)
            {
                g_foreign = true;
                g_written = false;
                Quietly([&] {
                    platform::LogWarning(L"column layout: the engine's three-column layout holds neither its stock values nor one of "
                                         L"the mod's ({}); left alone",
                                         RecordsText(entry));
                });
            }
            break;
        case Action::Nothing:
            g_written = ours;  // the mod's already, or the stock one wanted
            g_fault   = nullptr;
            break;
    }
}

/// A failed read or write of a two-player entry, logged once per reason.
void TwoFault(TwoEntry& e, const wchar_t* why) noexcept
{
    if (e.fault != why)
    {
        e.fault = why;
        platform::LogWarning(L"column layout: the engine's two-player layout ({}): {}", e.name, why);
    }
}

/// A two-player entry as it should be: `wanted` written, the stock one restored, or left alone (logged).
void ApplyTwo(TwoEntry& e, const TwoRecords& wanted) noexcept
{
    e.wanted               = wanted;
    const auto&    stock   = *e.stock;
    const bool     ours    = !column_rules::Same(wanted.record, stock);
    const wchar_t* why     = L"";
    std::uint8_t*  records = FindRecords(e.type, kTwoPlayerRecords, &why);
    if (records == nullptr)
    {
        Waiting(e.waiting, why);
        return;
    }
    e.waiting = nullptr;
    e.reached = true;
    Record entry[kTwoPlayerRecords]{};
    if (!platform::CopyGuarded(entry, records, sizeof(entry)))
    {
        TwoFault(e, L"could not be read; tried again twice a second");
        return;
    }
    const Action action = column_rules::DecideTwo(entry, wanted.record, stock, e.base.record, e.lastWrite.record);
    if (action != Action::Foreign && e.foreign)
    {
        e.foreign = false;
        platform::Log(L"column layout: the engine's two-player layout ({}) is {} again", e.name,
                      column_rules::Same(entry, stock) ? L"the stock one" : L"one of the mod's");
    }
    switch (action)
    {
        case Action::Write:
            if (platform::WriteGuarded(records, wanted.record, sizeof(wanted.record)))
            {
                e.written   = true;
                e.lastWrite = wanted;
                e.fault     = nullptr;
                ++e.writes;
                Quietly([&] {
                    platform::Log(L"column layout: the engine's two-player layout ({}) now shows {} in the screen order {}{}{} (was "
                                  L"{}); the engine lays two local players out from it on the next redraw (write {})",
                                  e.name,
                                  e.widths[0] == 0 ? std::wstring(L"its halves")
                                                   : std::format(L"two columns, each its monitor's width ({} | {} px)", e.widths[0], e.widths[1]),
                                  g_order.player[0], g_order.player[1], g_order.player[2],
                                  column_rules::Same(entry, stock) ? L"the stock one" : L"another of the mod's", e.writes);
                });
            }
            else
            {
                TwoFault(e, L"could not be written; two players keep the engine's equal halves; tried again twice a second");
            }
            break;
        case Action::Restore:
            if (platform::WriteGuarded(records, stock, sizeof(entry)))
            {
                e.written   = false;
                e.lastWrite = TwoRecords{{stock[0], stock[1]}};
                e.fault     = nullptr;
                ++e.restores;
                platform::Log(L"column layout: the engine's two-player layout ({}) put back to equal halves (restore {})", e.name, e.restores);
            }
            else
            {
                TwoFault(e, L"could not be put back; it keeps the mod's records; tried again twice a second");
            }
            break;
        case Action::Foreign:
            e.fault = nullptr;
            if (!e.foreign)
            {
                e.foreign = true;
                e.written = false;
                Quietly([&] {
                    platform::LogWarning(L"column layout: the engine's two-player layout ({}) holds neither its stock values nor one "
                                         L"of the mod's ({}); left alone",
                                         e.name, RecordsText(entry));
                });
            }
            break;
        case Action::Nothing:
            e.written = ours;
            e.fault   = nullptr;
            break;
    }
}

/// A two-player entry's state for the reading.
std::wstring TwoReading(const TwoEntry& e)
{
    const auto&    stock   = *e.stock;
    const wchar_t* why     = L"";
    std::uint8_t*  records = g_installed ? FindRecords(e.type, kTwoPlayerRecords, &why) : nullptr;
    Record         entry[kTwoPlayerRecords]{};
    if (records == nullptr || !platform::CopyGuarded(entry, records, sizeof(entry)))
    {
        return std::format(L"{} unread", e.name);
    }
    const std::wstring what = column_rules::Same(entry, stock)                       ? std::wstring(L"stock")
                              : column_rules::ArrangementOf(entry, e.base.record)    ? std::format(L"the mod's ({} | {} px)", e.widths[0], e.widths[1])
                                                                                     : L"other: " + RecordsText(entry);
    return std::format(L"{} {} (writes {}, restores {}{})", e.name, what, e.writes, e.restores,
                       e.fault != nullptr ? std::format(L"; fault: {}", e.fault) : L"");
}

/// The three-player setting held while three local players are listed; each change logged.
void HoldThreeLayout(int localPlayers) noexcept
{
    const auto change = g_threeLayout.Update(localPlayers == kThreePlayerRecords, kThreeColumnsLayout);
    if (change.kind == players::HeldChange::Kind::None)
    {
        return;
    }
    Quietly([&] {
        if (const auto text = players::Describe(change, L"three columns", kThreeLayoutName); !text.empty())
        {
            platform::Log(L"{}", text);
        }
    });
}

/// The screen order in force from now (the settings file, the console, a player's move), logged.
void SetOrder(const Order& order, const wchar_t* why) noexcept
{
    g_order = order;
    Quietly([&] {
        platform::Log(L"column layout: the screen order is {} {} {}, left to right (by player number; {})", order.player[0],
                      order.player[1], order.player[2], why);
    });
}

/// Three players in one window from now, logged.
void SetThreeSplit(column_rules::ThreeSplit split, const wchar_t* why) noexcept
{
    g_threeSplit = split;
    g_oneWindow  = column_rules::ThreeSplitRecords(split);
    Quietly([&] {
        platform::Log(L"column layout: three players in one window: {} ({})", column_rules::ThreeSplitWord(split), why);
    });
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    // The file's words are the table's, so they parse.
    namespace settings = platform::settings;
    Quietly([&] {
        SetOrder(column_rules::ParseOrder(settings::Of(settings::kColumnOrder).word).value_or(column_rules::kDefaultOrder),
                 settings::Why(settings::kColumnOrder).c_str());
        SetThreeSplit(column_rules::ParseThreeSplit(settings::Of(settings::kThreePlayerSplit).word).value_or(column_rules::ThreeSplit::Top),
                      settings::Why(settings::kThreePlayerSplit).c_str());
    });
    const auto engineGlobal = players::game_globals::Address(players::game_globals::Global::Engine);
    if (!engineGlobal)
    {
        platform::LogWarning(L"column layout: GEngine is outside the image; the columns keep the engine's equal widths");
        return false;
    }
    g_engineGlobal = engineGlobal;
    g_installed    = true;
    for (TwoEntry& e : g_two)
    {
        e.wanted    = TwoRecords{{(*e.stock)[0], (*e.stock)[1]}};
        e.lastWrite = e.wanted;
        e.base      = e.wanted;
    }
    platform::Log(L"column layout: ready (each column its monitor's width on a span of monitors of different widths, through "
                  L"the engine's own layout table)");
    return true;
}

void Tick(int localPlayers, std::span<const std::int32_t> spanWidths, std::span<const std::int32_t> spanHeights,
          const ListSeats& seats, bool columns) noexcept
{
    HoldThreeLayout(localPlayers);
    if (!g_installed)
    {
        return;
    }
    ++g_ticks;
    g_localPlayers = localPlayers;
    g_views        = localPlayers;
    g_seats        = seats;
    // The columns' sizes: the span's monitors' own (three for the three-column entry, two for side by side), equal
    // otherwise. A list that does not match the span's columns is not used.
    const bool threeWide = spanWidths.size() == kThreePlayerRecords && spanHeights.size() == kThreePlayerRecords;
    const bool twoWide   = spanWidths.size() == kTwoPlayerRecords && spanHeights.size() == kTwoPlayerRecords;
    g_widths             = threeWide ? std::array<std::int32_t, kThreePlayerRecords>{spanWidths[0], spanWidths[1], spanWidths[2]}
                                     : std::array<std::int32_t, kThreePlayerRecords>{};
    const Records three  = threeWide ? column_rules::ColumnRecords(g_widths, std::array<std::int32_t, kThreePlayerRecords>{spanHeights[0], spanHeights[1], spanHeights[2]})
                                     : Records{};
    g_columnsBase        = threeWide && three.record[0].sizeX > 0.0f ? three : StockThree();
    g_spanned            = threeWide || columns;  // the span's columns, or the console's (equal thirds)
    for (TwoEntry& e : g_two)
    {
        const bool side = twoWide && e.type == facts::kSplitTypeTwoPlayerVertical;
        e.widths        = side ? std::array<std::int32_t, kTwoPlayerRecords>{spanWidths[0], spanWidths[1]} : std::array<std::int32_t, kTwoPlayerRecords>{};
        const TwoRecords records = side ? column_rules::ColumnRecords(e.widths, std::array<std::int32_t, kTwoPlayerRecords>{spanHeights[0], spanHeights[1]})
                                        : TwoRecords{};
        e.base = side && records.record[0].sizeX > 0.0f ? records : TwoRecords{{(*e.stock)[0], (*e.stock)[1]}};
    }
    // The three-player entry: the span's columns, or the one-window layout, arranged by the screen order while three local
    // players are listed; the stock records otherwise.
    const Records wanted  = column_rules::ThreeWanted(g_spanned, g_columnsBase.record, g_oneWindow.record, localPlayers, g_order, seats);
    const bool    ours    = !column_rules::Same(wanted.record, kStockThreeColumns);
    const bool    change  = !column_rules::Same(wanted.record, g_wanted.record) && !g_foreign && g_fault == nullptr;
    const bool    cadence = g_ticks % kCheckTicks == 0 && (g_written || g_foreign || g_fault != nullptr || ours);
    if (change || cadence || !g_reached)
    {
        Apply(wanted);
    }
    // The two-player entries: the side-by-side one takes two monitors' widths while two local players are listed.
    for (TwoEntry& e : g_two)
    {
        const TwoRecords two      = column_rules::TwoWanted(e.base.record, *e.stock, localPlayers, g_order, seats);
        const bool       twoOurs  = !column_rules::Same(two.record, *e.stock);
        const bool       twoNew   = !column_rules::Same(two.record, e.wanted.record) && !e.foreign && e.fault == nullptr;
        const bool       twoCheck = g_ticks % kCheckTicks == 0 && (e.written || e.foreign || e.fault != nullptr || twoOurs);
        if (twoNew || twoCheck || !e.reached)
        {
            ApplyTwo(e, two);
        }
    }
    if (g_written && g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        try
        {
            platform::LogDebug(L"column layout: {}", Reading());
        }
        catch (...)
        {
        }
    }
}

void Remove() noexcept
{
    if (g_installed)
    {
        Apply(StockThree());  // the restore needs one of the mod's entries in it
        for (TwoEntry& e : g_two)
        {
            ApplyTwo(e, TwoRecords{{(*e.stock)[0], (*e.stock)[1]}});
        }
    }
    HoldThreeLayout(0);
}

Order CurrentOrder() noexcept
{
    return g_order;
}

int Views() noexcept
{
    return g_views;
}

ListSeats Seats() noexcept
{
    return g_seats;
}

void ChangeOrder(const Order& order, const wchar_t* why) noexcept
{
    if (!(order == g_order))
    {
        SetOrder(order, why);
    }
}

std::wstring Reading()
{
    const wchar_t* why     = L"";
    std::uint8_t*  records = g_installed ? FindRecords(&why) : nullptr;
    Record         entry[kThreePlayerRecords]{};
    std::wstring   table = L"unread";
    if (records != nullptr && platform::CopyGuarded(entry, records, sizeof(entry)))
    {
        table = column_rules::Same(entry, kStockThreeColumns)                ? std::wstring(L"stock (three equal columns)")
                : column_rules::ArrangementOf(entry, g_columnsBase.record) || column_rules::ArrangementOf(entry, g_oneWindow.record)
                    ? L"the mod's " + ColumnsText()
                                                                           : std::format(L"other: {}", RecordsText(entry));
    }
    return std::format(L"{} local player(s); the screen order {}{}{}; three in one window: {}; the span's columns: {}; the engine's "
                       L"three-player layout: {}{}; the three-player setting held at its three-column entry: {}; writes {}, restores {}{}; "
                       L"the two-player layouts: {}; {}",
                       g_localPlayers, g_order.player[0], g_order.player[1], g_order.player[2], column_rules::ThreeSplitWord(g_threeSplit), g_widths[0] == 0 ? std::wstring(L"none") : std::format(L"{} | {} | {} px", g_widths[0], g_widths[1], g_widths[2]),
                       table, records == nullptr && g_installed ? std::format(L" ({})", why) : L"", g_threeLayout.In() ? L"yes" : L"no",
                       g_writes, g_restores, g_fault != nullptr ? std::format(L"; fault: {}", g_fault) : L"", TwoReading(g_two[0]),
                       TwoReading(g_two[1]));
}

void RegisterOptions()
{
    namespace settings = platform::settings;
    control::RegisterOption(
        settings::kColumnOrder,
        [](const std::wstring& word) {
            const auto order = column_rules::ParseOrder(word);
            if (!order)
            {
                return control::Outcome{false, L"the order must be 1, 2 and 3, each once"};  // the shape checks the words
            }
            SetOrder(*order, L"the console");
            return control::Outcome{};
        },
        [] { return std::format(L"{}{}{}", g_order.player[0], g_order.player[1], g_order.player[2]); });
    control::RegisterOption(
        settings::kThreePlayerSplit,
        [](const std::wstring& word) {
            const auto split = column_rules::ParseThreeSplit(word);
            if (!split)
            {
                return control::Outcome{false, L"not a layout"};  // the shape checks the words
            }
            SetThreeSplit(*split, L"the console");
            return control::Outcome{};
        },
        [] { return std::wstring(column_rules::ThreeSplitWord(g_threeSplit)); });
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"column-layout.state",
                       .shape = control::Shape::Text,
                       .help  = L"the local players listed, the span's columns, the engine's three-column layout entry (stock, or the "
                                L"mod's columns of the monitors' widths), whether the three-player setting is held, and the two "
                                L"two-player entries",
                       .get   = [] { return control::Value{Reading()}; }});
}

#endif

}  // namespace lss::views::column_layout
