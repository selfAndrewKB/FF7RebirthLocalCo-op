#pragma once

// The mod's own settings file: LocalSplitScreen.ini in the mod's folder (Mods/LocalSplitScreen/LocalSplitScreen.ini,
// one level above the dlls folder this module is loaded from: platform/module_folder.h), read with the Windows
// profile API, so a user sets a preset once without the console. The options are one table here (the section and key
// in the file, the words the option takes, the word taken when the file has none, one line of help). Load reads every
// option once at the mod's start: one warning per word the file gets wrong, in one format, and one Info line with
// every option's word and where it came from. Each option's owner applies its reading at install through the one
// setter the console and the keys call too, and control::RegisterOption builds the console command from the table,
// so an option is described once. A word is read trimmed, lower-cased and with its spaces and commas removed (`1 3 2`,
// `1, 3, 2` and `132` are one word).
// A missing file or key is the default. The file is the table's own: Load writes it from the table after reading it
// (platform/settings_write.h: every option with its Co-op page name, help, words and default, the values kept, a key
// the table no longer has dropped), so nothing in it is a hand-kept copy that can go stale; the repository ships no
// copy. The Co-op page saves each choice it applies the same way (Save); the console and the keys change an option for
// the session only. Judge, WordList, WhyText, InvalidText and the file text and write need no game and are tested on
// the host (tests/settings_test.cpp). Game thread. A row is in the table once an owner answers it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lss::platform::settings {

/// The settings file's name, in the mod's folder.
inline constexpr const wchar_t* kFileName = L"LocalSplitScreen.ini";

/// One option of the file, as the table lists it.
struct Option
{
    const wchar_t*                  section;   // the [Section] in the file
    const wchar_t*                  key;       // the key in the file, and the console command's name
    std::span<const wchar_t* const> words;     // the words the option takes, lower case, in the help's order
    const wchar_t*                  fallback;  // the word taken when the file has none: one of `words`
    const wchar_t*                  help;      // one line, the console's help
    bool                            experimental = false;  // the mod menu shows it as experimental
    // The Co-op page (menu/coop_menu_rules.h): the row's name, each word's name in `words`' order, a short help line.
    const wchar_t*                  label = L"";
    std::span<const wchar_t* const> wordLabels{};
    const wchar_t*                  menuHelp = L"";
    // Set by the setup app, no row on the Co-op page: `words` is empty and the option takes any word, an empty one its default.
    bool                            appOnly = false;
};

// The table. An option's owner (the module named beside it) applies the reading at its install.
inline constexpr std::array<const wchar_t*, 2> kTwoPlayerSplitWords{L"vertical", L"horizontal"};
inline constexpr std::array<const wchar_t*, 2> kTwoPlayerSplitLabels{L"Vertical", L"Horizontal"};
inline constexpr std::array<const wchar_t*, 2> kOnOffWords{L"on", L"off"};
inline constexpr std::array<const wchar_t*, 2> kOnOffLabels{L"On", L"Off"};

inline constexpr Option kMultiMonitor{  // views/window_span
    L"Views", L"multi-monitor", kOnOffWords, L"off",
    L"the game window across a row of three equal monitors, one full view per column, whatever the players: on (held "
    L"from the game's first steady frame, as Ctrl+F3 does; a column no player fills shows player 1's picture), off (no "
    L"span until window-span on or Ctrl+F2 with a player in, or Ctrl+F3; from the console: leaves now)",
    false, L"Multiple Monitors", kOnOffLabels,
    L"One full view per player across a row of monitors, each fitted to its monitor. On: always. Off: "
    L"every view in one window."};
inline constexpr Option kMonitors{  // views/window_span (read by the monitor row once it picks monitors by name)
    L"Views", L"monitors", {}, L"",
    L"(read only) the monitors of the row, left to right, as the setup app wrote them; empty: any row the desktop has",
    false, L"Monitors in the Row", {},
    L"The monitors of the row, left to right, chosen in the Multi Monitor Setup app; they are moved into a row while the game holds it and back afterwards. Empty: the longest row the desktop has.",
    true};
inline constexpr Option kTwoPlayerSplit{  // players/split_layout
    L"Views", L"two-player-split", kTwoPlayerSplitWords, L"horizontal",
    L"two players in one window: vertical (side by side) or horizontal (top and bottom); a span of monitors is always side "
    L"by side (Ctrl+F5 switches it for the session)",
    false, L"2-Player Split-Screen Layout", kTwoPlayerSplitLabels,
    L"Vertical: side by side. Horizontal: one above the other."};

// The split FOV's weight (views/view_shape.h SplitFov): 0 to 1 in steps of 0.05, written "0.35".
inline constexpr std::size_t kFovWeightSteps = 21;
using FovWeightText = std::array<std::array<wchar_t, 5>, kFovWeightSteps>;
consteval FovWeightText MakeFovWeightText()
{
    FovWeightText text{};
    for (std::size_t i = 0; i < kFovWeightSteps; ++i)
    {
        const std::size_t hundredths = i * 5;
        text[i][0] = static_cast<wchar_t>(L'0' + hundredths / 100);
        text[i][1] = L'.';
        text[i][2] = static_cast<wchar_t>(L'0' + hundredths / 10 % 10);
        text[i][3] = static_cast<wchar_t>(L'0' + hundredths % 10);
        text[i][4] = L'\0';
    }
    return text;
}
inline constexpr FovWeightText kFovWeightText = MakeFovWeightText();
consteval std::array<const wchar_t*, kFovWeightSteps> MakeFovWeightWords()
{
    std::array<const wchar_t*, kFovWeightSteps> words{};
    for (std::size_t i = 0; i < kFovWeightSteps; ++i)
    {
        words[i] = kFovWeightText[i].data();
    }
    return words;
}
/// "0.00" ... "1.00": the words and the page's values alike.
inline constexpr std::array<const wchar_t*, kFovWeightSteps> kFovWeightWords = MakeFovWeightWords();

inline constexpr Option kHorizontalFov{  // views/player_view_point
    L"Views", L"split-fov.horizontal", kFovWeightWords, L"0.50",
    L"how a filled view wider than the game's image shares the difference (two players top and bottom): 0.00 keeps the "
    L"single-player height (the whole picture's height shown: everything smaller, more to the sides), 1.00 the "
    L"single-player size (as wide as in single player, cut at the knees), 0.50 the middle; steps of 0.05",
    false, L"Horizontal Split-Screen FOV", kFovWeightWords,
    L"For views wider than the game's picture. Low: more of the scene, everything smaller. High: characters at their usual size, cut off "
    L"lower down. 0.50: the middle."};

/// Every option, in the order the settings file lists them.
inline constexpr std::array kOptions{&kMultiMonitor, &kMonitors, &kTwoPlayerSplit, &kHorizontalFov};

/// The option is a row of the table (Of and Why answer by the row).
consteval bool Listed(const Option& option)
{
    for (const Option* row : kOptions)
    {
        if (row == &option) return true;
    }
    return false;
}
static_assert(Listed(kMultiMonitor) && Listed(kMonitors) && Listed(kTwoPlayerSplit) && Listed(kHorizontalFov),
              "every option is a row of kOptions");

/// Where an option's word came from.
enum class Source : std::uint8_t
{
    Default,  // the file has no such key (or no file): the option's fallback
    File,     // the file's word, one of the option's
    Invalid,  // the file's word is none of the option's: the fallback, with a warning
};

struct Reading
{
    std::wstring word;                      // one of the option's words
    Source       source = Source::Default;
    std::wstring raw;                       // the file's text as written (trimmed), kept for the warning when Invalid
};

/// The file's text for an option, judged: trimmed, lower-cased and with its spaces and commas removed, then matched
/// against the option's words; empty is the default.
inline Reading Judge(const Option& option, std::wstring_view fileText)
{
    std::wstring word;
    for (const wchar_t c : fileText)
    {
        if (!std::iswspace(c) && c != L',')
        {
            word.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
    }
    if (word.empty())
    {
        return {option.fallback, Source::Default, {}};
    }
    if (option.words.empty())
    {
        return {word, Source::File, {}};
    }
    for (const wchar_t* allowed : option.words)
    {
        if (word == allowed)
        {
            return {word, Source::File, {}};
        }
    }
    std::size_t first = 0, last = fileText.size();
    while (first < last && std::iswspace(fileText[first])) ++first;
    while (last > first && std::iswspace(fileText[last - 1])) --last;
    return {option.fallback, Source::Invalid, std::wstring(fileText.substr(first, last - first))};
}

/// The place of `word` among the option's words, or nothing: an owner that keeps a number per word (the FOV weight
/// by its step) maps through it, so no locale's decimal mark is read.
inline std::optional<std::size_t> WordIndex(const Option& option, std::wstring_view word) noexcept
{
    for (std::size_t i = 0; i < option.words.size(); ++i)
    {
        if (word == option.words[i])
        {
            return i;
        }
    }
    return std::nullopt;
}

/// The option's words as a list: "auto, on or off".
inline std::wstring WordList(const Option& option)
{
    std::wstring list;
    const std::size_t count = option.words.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        list += option.words[i];
        if (i + 2 < count) list += L", ";
        else if (i + 2 == count) list += L" or ";
    }
    return list;
}

/// The warning Load logs for an invalid word: "LocalSplitScreen.ini [Views] multi-monitor = 'x' is not on or off; read
/// as off".
inline std::wstring InvalidText(const Option& option, const Reading& reading)
{
    return std::wstring(L"LocalSplitScreen.ini [") + option.section + L"] " + option.key + L" = '" + reading.raw +
           L"' is not " + WordList(option) + L"; read as " + option.fallback;
}

/// Why a reading reads as it does, for the owner's own log line: "the settings file", "the default; LocalSplitScreen.ini
/// has no [Views] multi-monitor", "the default; LocalSplitScreen.ini [Views] multi-monitor = 'x' is not on or off".
inline std::wstring WhyText(const Option& option, const Reading& reading)
{
    switch (reading.source)
    {
        case Source::File:
            return L"the settings file";
        case Source::Invalid:
            return std::wstring(L"the default; LocalSplitScreen.ini [") + option.section + L"] " + option.key + L" = '" +
                   reading.raw + L"' is not " + WordList(option);
        case Source::Default:
        default:
            return std::wstring(L"the default; LocalSplitScreen.ini has no [") + option.section + L"] " + option.key;
    }
}

/// The file's path, in the mod's folder (platform::ModFolder); empty if the module's path cannot be read.
std::wstring Path();

/// Reads every option of the table from the file, once, before any module installs (game thread). A missing file
/// leaves every option at its default. Logs one warning per invalid word and one Info line with every reading. Then
/// writes the file from the table with those readings, unless it already reads exactly so (logged at Info, with any key
/// dropped). A failure to read or write (out of memory) is logged; the readings stand as they are.
void Load() noexcept;

/// An option's reading (its default before Load). An option that is not a row of the table (a programming error,
/// caught by Listed's static_assert for the rows declared here) answers its own fallback, with a warning once.
const Reading& Of(const Option& option) noexcept;

/// WhyText(option, Of(option)).
std::wstring Why(const Option& option);

/// Writes the file from the table with `word` (one of the option's words) for `option`, so the next start reads it,
/// and makes it the option's reading (from the file). Logs the save (Info) or why it failed (Warning). False if the word is none of the
/// option's, the option is not a row of the table, the file's path is unknown or the write failed.
bool Save(const Option& option, const std::wstring& word);

}  // namespace lss::platform::settings
