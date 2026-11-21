// Host-side tests for the Co-op page's rules (menu/coop_menu_rules.h): the page built from the settings table, the
// row bytes kept apart from the game's own settings, the values and words, and the console's readings.

#include "menu/coop_menu_rules.h"

#include "expect.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace lss::menu::coop_menu_rules;
namespace settings = lss::platform::settings;
namespace control  = lss::control;

/// Every option on the page has a row name, a short help and one name per word: the page shows nothing the table lacks.
void EveryOptionHasMenuText()
{
    std::puts("coop menu: every row has a name, a help line and a name per word");
    for (const settings::Option* option : settings::kOptions)
    {
        if (option->appOnly)
        {
            continue;
        }
        EXPECT(std::wcslen(option->label) > 0);
        EXPECT(std::wcslen(option->menuHelp) > 0);
        EXPECT(option->wordLabels.size() == option->words.size());
        for (const wchar_t* name : option->wordLabels)
        {
            EXPECT(std::wcslen(name) > 0);
        }
    }
}

/// One category per section in the table's order, on the screen's keys (never 5 or 6), every option on exactly one row.
void PageFromTheTable()
{
    std::puts("coop menu: one category per section, every option on one row");
    const auto page = BuildPage();
    EXPECT(page.size() == 3);  // Screens, Battle, Experimental
    EXPECT(page.size() <= kCategoryKeys.size());
    std::set<int>          rows;
    std::set<std::uint8_t> keys;
    for (std::size_t i = 0; i < page.size(); ++i)
    {
        EXPECT(page[i].key == kCategoryKeys[i]);
        EXPECT(page[i].key != 5 && page[i].key != 6);
        EXPECT(keys.insert(page[i].key).second);
        EXPECT(!page[i].options.empty());
        EXPECT(std::wcslen(page[i].text.help) > 0);
        for (const int option : page[i].options)
        {
            EXPECT(rows.insert(option).second);
            EXPECT(std::wcscmp(settings::kOptions[static_cast<std::size_t>(option)]->section, page[i].text.section) == 0);
        }
    }
    std::size_t onPage = 0;
    for (const settings::Option* option : settings::kOptions)
    {
        onPage += option->appOnly || settings::OnGameScreen(*option) ? 0 : 1;
    }
    EXPECT(rows.size() == onPage);
    EXPECT(std::wcscmp(page[0].text.label, L"Screens") == 0);
    EXPECT(page[0].options == (std::vector<int>{0, 2, 3, 4, 5, 6, 7}));  // 1 is the monitors, set by the setup app
}

/// A row's byte is above every game setting (0..0x54) and names its option back; game bytes name none.
void RowBytesApartFromTheGame()
{
    std::puts("coop menu: the rows' bytes are above the game's settings");
    for (int option = 0; option < static_cast<int>(settings::kOptions.size()); ++option)
    {
        EXPECT(RowByte(option) >= 0x80);
        EXPECT(OptionOf(RowByte(option)) == option);
    }
    for (int game = 0; game <= 0x54; ++game)
    {
        EXPECT(!OptionOf(static_cast<std::uint8_t>(game)).has_value());
    }
    EXPECT(!OptionOf(static_cast<std::uint8_t>(0x7F)).has_value());
    EXPECT(!OptionOf(RowByte(static_cast<int>(settings::kOptions.size()))).has_value());
    EXPECT(!OptionOf(0xFF).has_value());
}

/// Values are the words' places; a value outside them commits nothing; an unknown word has no value.
void ValuesAndWords()
{
    std::puts("coop menu: values are the words' places");
    const settings::Option& order = settings::kColumnOrder;
    EXPECT(ValueOf(order, L"123") == 0);
    EXPECT(ValueOf(order, L"321") == 5);
    EXPECT(!ValueOf(order, L"nonsense").has_value());
    EXPECT(WordOf(order, 2) == std::wstring(L"213"));
    EXPECT(!WordOf(order, 6).has_value());
    EXPECT(!WordOf(order, -1).has_value());
    for (const settings::Option* option : settings::kOptions)
    {
        for (int value = 0; value < static_cast<int>(option->words.size()); ++value)
        {
            EXPECT(ValueOf(*option, *WordOf(*option, value)) == value);
        }
    }
}

/// An on/off option's command takes and reads a bool; any other takes and reads its word.
void CommandValues()
{
    std::puts("coop menu: on/off commands take a bool, the rest their word");
    EXPECT(std::get<bool>(CommandValue(control::Shape::OnOff, L"on")));
    EXPECT(!std::get<bool>(CommandValue(control::Shape::OnOff, L"off")));
    EXPECT(std::get<std::wstring>(CommandValue(control::Shape::Word, L"top")) == L"top");
    EXPECT(WordOfReading(control::Value{true}) == std::wstring(L"on"));
    EXPECT(WordOfReading(control::Value{false}) == std::wstring(L"off"));
    EXPECT(WordOfReading(control::Value{std::wstring(L"132")}) == std::wstring(L"132"));
    EXPECT(!WordOfReading(control::Value{std::int64_t{3}}).has_value());
    EXPECT(!WordOfReading(control::Value{}).has_value());
}

/// The split FOV's two weights (21 values each), the HUD size (16) and the six battle rates (100 each) are sliders; the
/// short lists stay lists.
void SlidersAreTheLongRows()
{
    std::puts("coop menu: the long rows are sliders");
    int sliders = 0;
    for (const settings::Option* option : settings::kOptions)
    {
        EXPECT(IsSlider(*option) == (option->wordLabels.size() > kListLongest));
        sliders += IsSlider(*option) ? 1 : 0;
    }
    EXPECT(sliders == 9);
}

/// Reset All gives each row its option's default word.
void ResetGivesDefaults()
{
    std::puts("coop menu: Reset All gives each row its default");
    for (const settings::Option* option : settings::kOptions)
    {
        if (option->words.empty())
        {
            continue;
        }
        const int value = DefaultValue(*option);
        EXPECT(value >= 0 && value < static_cast<int>(option->words.size()));
        EXPECT(std::wcscmp(option->words[static_cast<std::size_t>(value)], option->fallback) == 0);
    }
}

/// A slider's arrows move right by its longest word's characters past the game's two digits; short words move nothing.
void LabelRoom()
{
    std::puts("coop menu: a slider's arrows make room for its words");
    for (const settings::Option* option : settings::kOptions)
    {
        if (IsSlider(*option))
        {
            // The weights' values "0.00" to "1.00" (two characters past the digits), the HUD size's "x0.75" to "x1.50"
            // and the battle's rates "x0.1" to "x10.0" (three).
            std::size_t longest = 0;
            for (const wchar_t* label : option->wordLabels)
            {
                longest = std::max(longest, std::wcslen(label));
            }
            EXPECT(longest == 4 || longest == 5);
            EXPECT(LabelShift(*option) == static_cast<float>(longest - kGaugeLabelDigits) * kGaugeCharWidth);
        }
    }
    constexpr const wchar_t* kShort[] = {L"1", L"10"};
    settings::Option         two{};
    two.wordLabels = kShort;
    EXPECT(LabelShift(two) == 0.0f);
}

/// The window follows the marker with a cell of context, stays inside the gauge, and is 0 when everything fits.
void SliderWindow()
{
    std::puts("coop menu: a slider's window follows its marker");
    constexpr int cells = 20, window = kSliderWindow;
    EXPECT(FirstVisible(9, 0, 10, window) == 0);  // a stock gauge: 10 cells, no window
    EXPECT(FirstVisible(0, 0, cells, window) == 0);
    EXPECT(FirstVisible(9, 0, cells, window) == 0);
    EXPECT(FirstVisible(10, 0, cells, window) == 1);  // one step on: the window moves one cell
    EXPECT(FirstVisible(15, 0, cells, window) == 6);
    EXPECT(FirstVisible(14, 6, cells, window) == 6);  // back inside: no move
    EXPECT(FirstVisible(6, 6, cells, window) == 5);   // within a cell of the left edge
    EXPECT(FirstVisible(20, 6, cells, window) == 10);  // the end: never past the last cell
    EXPECT(FirstVisible(0, 10, cells, window) == 0);   // back to the start
    for (int marker = 0; marker <= cells; ++marker)
    {
        for (const int first : {0, 5, 10})
        {
            const int shown = FirstVisible(marker, first, cells, window);
            EXPECT(shown >= 0 && shown <= cells - window && marker >= shown && marker <= shown + window);
        }
    }
}

/// Players 2 and 3's Responsiveness are the game screen's rows, not the page's: Range rows shown from 1 (the game's own
/// 1 to 5), every word's value naming it back, values outside committing nothing.
void GameScreenRows()
{
    std::puts("coop menu: players 2 and 3's Responsiveness are rows of the game's own Options screen");
    const auto game = GameScreenOptions();
    EXPECT(game.size() == 2);
    for (const int option : game)
    {
        const settings::Option& row = *settings::kOptions[static_cast<std::size_t>(option)];
        EXPECT(RowMin(row) == 1);
        EXPECT(ShownOf(row, L"1") == 1);
        EXPECT(ShownOf(row, L"5") == 5);
        EXPECT(WordAt(row, 3) == std::wstring(L"3"));
        EXPECT(!WordAt(row, 0).has_value());
        EXPECT(!WordAt(row, 6).has_value());
        for (int value = 1; value <= 5; ++value)
        {
            EXPECT(ShownOf(row, *WordAt(row, value)) == value);
        }
    }
    EXPECT(game.size() == 2 && settings::kOptions[static_cast<std::size_t>(game[0])] == &settings::kResponsivenessTwo &&
           settings::kOptions[static_cast<std::size_t>(game[1])] == &settings::kResponsivenessThree);
    for (const auto& category : BuildPage())
    {
        for (const int option : category.options)
        {
            EXPECT(RowMin(*settings::kOptions[static_cast<std::size_t>(option)]) == 0);
        }
    }
}

/// The game screen's rows go right after the game's row, once: never into rows that already hold one of the mod's, never
/// without that row. Camera & Controls as the construct leaves it, Responsiveness (15) the eighth row.
void GameRowsPlace()
{
    std::puts("coop menu: the game screen's rows go right after Responsiveness, once");
    constexpr std::uint8_t          responsiveness = 15;
    const std::vector<std::uint8_t> camera{9, 10, 11, 12, 13, 14, 50, 15, 80, 16, 17, 18, 19, 20, 23};
    EXPECT(GameRowsAt(camera, responsiveness) == std::size_t{8});
    std::vector<std::uint8_t> given = camera;
    given.insert(given.begin() + 8, RowByte(GameScreenOptions()[0]));
    EXPECT(!GameRowsAt(given, responsiveness).has_value());
    const std::vector<std::uint8_t> without{9, 10, 16, 17};
    EXPECT(!GameRowsAt(without, responsiveness).has_value());
    EXPECT(!GameRowsAt({}, responsiveness).has_value());
    const std::vector<std::uint8_t> last{14, 15};
    EXPECT(GameRowsAt(last, responsiveness) == std::size_t{2});
    // A row of the mod's anywhere, even past the game's row, means the rows are there.
    std::vector<std::uint8_t> laterMod = camera;
    laterMod.push_back(RowByte(GameScreenOptions()[1]));
    EXPECT(!GameRowsAt(laterMod, responsiveness).has_value());
    // Two rows of the game's byte: after the first.
    const std::vector<std::uint8_t> twice{14, 15, 16, 15};
    EXPECT(GameRowsAt(twice, responsiveness) == std::size_t{2});
}

}  // namespace

int main()
{
    SlidersAreTheLongRows();
    LabelRoom();
    ResetGivesDefaults();
    SliderWindow();
    EveryOptionHasMenuText();
    PageFromTheTable();
    RowBytesApartFromTheGame();
    ValuesAndWords();
    CommandValues();
    GameScreenRows();
    GameRowsPlace();
    return lss::test::Report();
}
