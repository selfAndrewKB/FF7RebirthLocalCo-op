// Host-side tests for the settings table (platform/settings.h): the table's own consistency, the judging of a
// file's text (trim, case, spaces, the default, an invalid word), the word list and the texts the owners and the
// warning use, the word index the owners map numbers through, and the file the mod writes from the table
// (platform/settings_write.h), as text and on a temporary file read back as Load reads it. Run without the game
// (Load, Of, Why, Save and Path are the game side, not built here).

#include "platform/settings.h"
#include "platform/settings_write.h"

#include "expect.h"

#include <array>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

namespace {

using namespace lss::platform::settings;

/// Every option: at least two words, a fallback among them, a section, a key and help; the keys unique (they are
/// the console's command names).
void TableConsistent()
{
    std::puts("settings: the table is consistent");
    std::set<std::wstring> keys;
    for (const Option* option : kOptions)
    {
        EXPECT(option->words.size() >= 2 || option->appOnly);
        EXPECT(option->words.empty() == option->appOnly);
        bool fallbackAllowed = option->appOnly;  // any word, the empty one its default
        for (const wchar_t* word : option->words)
        {
            fallbackAllowed = fallbackAllowed || std::wcscmp(word, option->fallback) == 0;
            EXPECT(Judge(*option, word).word == word);  // every word is its own lower-case, space-free form
        }
        EXPECT(fallbackAllowed);
        EXPECT(std::wcslen(option->section) > 0);
        EXPECT(std::wcslen(option->key) > 0);
        EXPECT(std::wcslen(option->help) > 0);
        EXPECT(std::wcslen(option->label) > 0);     // the file names each option by its Co-op page row
        EXPECT(std::wcslen(option->menuHelp) > 0);  // and describes it with the page's help
        EXPECT(keys.insert(option->key).second);
    }
    // Each section's options are together in the table, so the file lists each section once.
    std::set<std::wstring> closed;
    std::wstring           open;
    for (const Option* option : kOptions)
    {
        if (open != option->section)
        {
            EXPECT(closed.insert(option->section).second);
            open = option->section;
        }
    }
    EXPECT(kOptions.size() == 15);
    // The six battle rates: multipliers from 0.1 to 10.0 in tenths, each default the game's own 1.0.
    for (const Option* rate : {&kEnemyDamage, &kEnemyHp, &kBossHp, &kEnemyStagger, &kExpGain, &kApGain})
    {
        EXPECT(rate->words.size() == 100 && std::wcscmp(rate->fallback, L"1.0") == 0);
        EXPECT(std::wcscmp(rate->words[0], L"0.1") == 0 && std::wcscmp(rate->words[99], L"10.0") == 0);
        EXPECT(std::wstring_view(rate->section) == L"Battle");
    }
    EXPECT(kLeadHolder.words.size() == 2 && std::wcscmp(kLeadHolder.fallback, L"guide") == 0 && kLeadHolder.experimental);
    EXPECT(kBattleWarps.words.size() == 3 && std::wcscmp(kBattleWarps.fallback, L"players-stay") == 0);
    EXPECT(kVerticalFov.words.size() == kFovWeightSteps && std::wcscmp(kVerticalFov.fallback, L"1.00") == 0);
    // The screen order's six words, by player number, the default 123; the three-player layouts in one window (no
    // columns: three equal columns are the span's only).
    EXPECT(kColumnOrder.words.size() == 6 && std::wcscmp(kColumnOrder.fallback, L"123") == 0);
    EXPECT(kThreePlayerSplit.words.size() == 4 && std::wcscmp(kThreePlayerSplit.fallback, L"top") == 0);
    // The two-player layout's words are the engine's: vertical = side by side, horizontal = top/bottom, the default.
    EXPECT(std::wcscmp(kTwoPlayerSplit.words[0], L"vertical") == 0 && std::wcscmp(kTwoPlayerSplit.wordLabels[0], L"Vertical") == 0);
    EXPECT(std::wcscmp(kTwoPlayerSplit.fallback, L"horizontal") == 0);
    // The FOV weight: 21 words from 0.00 to 1.00 in steps of 0.05, the default the middle.
    EXPECT(kHorizontalFov.words.size() == kFovWeightSteps);
    EXPECT(std::wcscmp(kHorizontalFov.words[0], L"0.00") == 0);
    EXPECT(std::wcscmp(kHorizontalFov.words[1], L"0.05") == 0);
    EXPECT(std::wcscmp(kHorizontalFov.words[10], L"0.50") == 0);
    EXPECT(std::wcscmp(kHorizontalFov.words[20], L"1.00") == 0);
    EXPECT(std::wcscmp(kHorizontalFov.fallback, L"0.50") == 0);
    EXPECT(kMonitors.appOnly && kMonitors.words.empty() && std::wcslen(kMonitors.fallback) == 0);
}

void JudgeFile()
{
    std::puts("settings: a file's text is judged");
    // A word of the option's, as written and with the case, the spaces and the ends the user may add.
    EXPECT(Judge(kMultiMonitor, L"on").source == Source::File);
    EXPECT(Judge(kMultiMonitor, L"on").word == L"on");
    EXPECT(Judge(kMultiMonitor, L"  On \t").word == L"on");
    EXPECT(Judge(kMultiMonitor, L"  On \t").source == Source::File);
    EXPECT(Judge(kTwoPlayerSplit, L"Vertical").word == L"vertical");
    EXPECT(Judge(kHorizontalFov, L"0.50").word == L"0.50");
    EXPECT(Judge(kHorizontalFov, L"1 . 0 0").word == L"1.00");
    // No key, or an empty value: the default.
    EXPECT(Judge(kTwoPlayerSplit, L"").word == L"horizontal");
    EXPECT(Judge(kTwoPlayerSplit, L"").source == Source::Default);
    EXPECT(Judge(kTwoPlayerSplit, L"   ").source == Source::Default);
    // A word that is none of the option's: the default, the file's text kept for the warning.
    const Reading bad = Judge(kTwoPlayerSplit, L" sideways ");
    EXPECT(bad.source == Source::Invalid);
    EXPECT(bad.word == L"horizontal");
    EXPECT(bad.raw == L"sideways");
    EXPECT(Judge(kHorizontalFov, L"0.5").source == Source::Invalid);  // the words are written with two decimals
    EXPECT(Judge(kHorizontalFov, L"0.5").word == L"0.50");
    // An option of any word (the app's): the text as its lower-case, space-free, comma-free form.
    EXPECT(Judge(kMonitors, L"\\\\?\\DISPLAY#ONE#{a-b}|\\\\?\\DISPLAY#TWO#{a-b}").source == Source::File);
    EXPECT(Judge(kMonitors, L" One Two, Three ").word == L"onetwothree");
    EXPECT(Judge(kMonitors, L"").source == Source::Default && Judge(kMonitors, L"").word.empty());
}

void Texts()
{
    std::puts("settings: the word list, the warning and the owner's reason");
    EXPECT(WordList(kMultiMonitor) == L"on or off");
    EXPECT(WordList(kTwoPlayerSplit) == L"vertical or horizontal");
    EXPECT(WhyText(kTwoPlayerSplit, Judge(kTwoPlayerSplit, L"vertical")) == L"the settings file");
    EXPECT(WhyText(kTwoPlayerSplit, Judge(kTwoPlayerSplit, L"")) ==
           L"the default; LocalSplitScreen.ini has no [Views] two-player-split");
    EXPECT(WhyText(kTwoPlayerSplit, Judge(kTwoPlayerSplit, L"maybe")) ==
           L"the default; LocalSplitScreen.ini [Views] two-player-split = 'maybe' is not vertical or horizontal");
    EXPECT(InvalidText(kMultiMonitor, Judge(kMultiMonitor, L"yes")) ==
           L"LocalSplitScreen.ini [Views] multi-monitor = 'yes' is not on or off; read as off");
}

void WordIndexes()
{
    std::puts("settings: an owner maps a word through its place in the table");
    EXPECT(WordIndex(kHorizontalFov, L"0.00") == 0);
    EXPECT(WordIndex(kHorizontalFov, L"0.50") == 10);
    EXPECT(WordIndex(kHorizontalFov, L"1.00") == 20);
    EXPECT(!WordIndex(kHorizontalFov, L"0.5"));
    EXPECT(!WordIndex(kHorizontalFov, L""));
    EXPECT(WordIndex(kTwoPlayerSplit, L"horizontal") == 1);
    EXPECT(!WordIndex(kMonitors, L"anything"));
}

/// Every option at its default, as Load reads a missing file.
std::array<Reading, kOptions.size()> Defaults()
{
    std::array<Reading, kOptions.size()> readings;
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        readings[i] = Judge(*kOptions[i], L"");
    }
    return readings;
}

std::size_t Count(const std::wstring& text, const std::wstring& part)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::wstring::npos; at = text.find(part, at + 1))
    {
        ++count;
    }
    return count;
}

/// An option's place in the table, which the readings follow.
std::size_t PlaceOf(const Option& option)
{
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        if (kOptions[i] == &option)
        {
            return i;
        }
    }
    return kOptions.size();
}

/// The file's text from the table: every option once with its value line, each section once, plain ASCII, no line
/// past the width, a long run of values as a range, a word the mod cannot read kept as written with a note.
void FileTextFromTable()
{
    std::puts("settings: the file's text comes from the table");
    auto readings = Defaults();
    readings[PlaceOf(kTwoPlayerSplit)] = Judge(kTwoPlayerSplit, L"Vertical");
    readings[PlaceOf(kHorizontalFov)]  = Judge(kHorizontalFov, L"sometimes");
    const std::wstring text = FileText(readings);
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Option& option = *kOptions[i];
        EXPECT(Count(text, std::wstring(L"\r\n") + option.key + L" = ") == 1);
        EXPECT(Count(text, std::wstring(L"; ") + option.label + L": ") == 1);
        EXPECT(Count(text, std::wstring(L"[") + option.section + L"]\r\n") == 1);
    }
    EXPECT(text.find(L"\r\nmulti-monitor = off\r\n") != std::wstring::npos);
    EXPECT(text.find(L"\r\ntwo-player-split = vertical\r\n") != std::wstring::npos);
    EXPECT(text.find(L"\r\nsplit-fov.horizontal = sometimes\r\n") != std::wstring::npos);
    EXPECT(Count(text, L"none of these words: read as 0.50") == 1);
    EXPECT(text.find(L"; 0.00 to 1.00 (the default: 0.50)") != std::wstring::npos);
    EXPECT(text.find(L"; on | off (the default: off)") != std::wstring::npos);
    EXPECT(text.find(L"; vertical | horizontal (the default: horizontal)") != std::wstring::npos);
    EXPECT(text.find(L"LocalSplitScreen (FF7 Rebirth)") != std::wstring::npos);
    std::size_t lineStart = 0;
    bool        ascii     = true, narrow = true;
    for (std::size_t at = 0; at < text.size(); ++at)
    {
        ascii = ascii && text[at] < 0x80;
        if (text[at] == L'\n')
        {
            narrow    = narrow && at - lineStart <= kFileWidth + 1;  // the CR before the LF
            lineStart = at + 1;
        }
    }
    EXPECT(ascii);
    EXPECT(narrow);
    EXPECT(text.size() >= 2 && text.substr(text.size() - 2) == L"\r\n");
}

/// The value the file now holds for an option, read as Load reads it.
Reading ReadBack(const std::wstring& path, const Option& option)
{
    wchar_t    buffer[256]{};
    const auto length = GetPrivateProfileStringW(option.section, option.key, L"", buffer, 256, path.c_str());
    return Judge(option, std::wstring_view(buffer, length));
}

/// The written file on disk: read back as the next start reads it, every value as it was written; written again over
/// an old file whose comments and stale keys are gone.
void FileWrittenAndReadBack()
{
    std::puts("settings: the file is written and reads back");
    wchar_t folder[MAX_PATH]{};
    EXPECT(GetTempPathW(MAX_PATH, folder) > 0);
    const std::wstring path = std::wstring(folder) + L"lss_rebirth_settings_test.ini";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "; an old copy\r\n[Views]\r\nmulti-monitor = auto\r\nold-option = yes\r\n";
    }
    auto readings = Defaults();
    readings[PlaceOf(kMultiMonitor)]   = Judge(kMultiMonitor, L"on");
    readings[PlaceOf(kTwoPlayerSplit)] = Judge(kTwoPlayerSplit, L"vertical");
    readings[PlaceOf(kHorizontalFov)]  = Judge(kHorizontalFov, L"sometimes");
    const std::string bytes = Utf8(FileText(readings));
    EXPECT(WriteFileBytes(path.c_str(), bytes) == 0);
    EXPECT(FileBytes(path.c_str()) == bytes);
    for (std::size_t i = 0; i < kOptions.size(); ++i)
    {
        const Reading back = ReadBack(path, *kOptions[i]);
        // A default is written out and reads back as the file's, except an empty one, which is no word in the file.
        const bool emptyDefault = readings[i].source == Source::Default && readings[i].word.empty();
        EXPECT(back.word == readings[i].word &&
               back.source == (emptyDefault ? Source::Default : readings[i].source == Source::Default ? Source::File : readings[i].source));
    }
    EXPECT(ReadBack(path, kHorizontalFov).raw == L"sometimes");
    // An option of any word: lower-cased as the file reads it, written and read back whole.
    readings[PlaceOf(kMonitors)] = Judge(kMonitors, L"\\\\?\\DISPLAY#ONE#{a-b}|\\\\?\\DISPLAY#TWO#{a-b}");
    EXPECT(readings[PlaceOf(kMonitors)].source == Source::File);
    EXPECT(readings[PlaceOf(kMonitors)].word == L"\\\\?\\display#one#{a-b}|\\\\?\\display#two#{a-b}");
    EXPECT(WriteFileBytes(path.c_str(), Utf8(FileText(readings))) == 0);
    EXPECT(ReadBack(path, kMonitors).word == readings[PlaceOf(kMonitors)].word);
    readings[PlaceOf(kMonitors)] = Judge(kMonitors, L"");
    // A value with characters outside ASCII: kept with `?`, and a second start writes the same bytes (no drift).
    const std::wstring typed = std::wstring(L"0,5") + wchar_t{0x00D7} + L' ' + wchar_t{0x2713};  // "0,5x" and a tick
    readings[PlaceOf(kHorizontalFov)] = Judge(kHorizontalFov, typed);
    EXPECT(readings[PlaceOf(kHorizontalFov)].source == Source::Invalid);
    const std::string first = Utf8(FileText(readings));
    EXPECT(WriteFileBytes(path.c_str(), first) == 0);
    auto again                     = readings;
    again[PlaceOf(kHorizontalFov)] = ReadBack(path, kHorizontalFov);
    EXPECT(again[PlaceOf(kHorizontalFov)].source == Source::Invalid && again[PlaceOf(kHorizontalFov)].raw == L"0,5? ?");
    EXPECT(Utf8(FileText(again)) == first);
    const std::string text = FileBytes(path.c_str());
    EXPECT(text.find("an old copy") == std::string::npos);
    EXPECT(text.find("old-option") == std::string::npos);
    std::wstring temp = path + L".tmp";
    EXPECT(GetFileAttributesW(temp.c_str()) == INVALID_FILE_ATTRIBUTES);  // the temporary file moved, none left
    DeleteFileW(path.c_str());
}

}  // namespace

int main()
{
    TableConsistent();
    JudgeFile();
    Texts();
    WordIndexes();
    FileTextFromTable();
    FileWrittenAndReadBack();
    return lss::test::Report();
}
