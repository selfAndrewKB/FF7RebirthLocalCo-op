// Host-side tests for the command registry (platform/command_registry.h): the table's refusals, each shape's parsing
// and the values written back, run without the game.

#include "platform/command_registry.h"
#include "platform/settings.h"

#include "expect.h"

#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>

namespace {

using lss::control::Entry;
using lss::control::Format;
using lss::control::Outcome;
using lss::control::ParseValue;
using lss::control::Registry;
using lss::control::Shape;
using lss::control::Value;

Outcome Done(const Value&)
{
    return {};
}

Entry Action(const wchar_t* name)
{
    return Entry{.name = name, .set = Done};
}

bool Refused(const Entry& entry, const wchar_t* text)
{
    const auto parsed = ParseValue(entry, text);
    return !parsed.value && !parsed.error.empty();
}

/// The value the text parses to, written back: a canonical text comes back unchanged.
std::wstring RoundTrip(const Entry& entry, const wchar_t* text)
{
    const auto parsed = ParseValue(entry, text);
    return parsed.value ? Format(entry, *parsed.value) : L"(refused)";
}

void TestAddAndFind()
{
    std::puts("registry: entries are found by name, in registration order");
    Registry registry;
    EXPECT(registry.Add(Action(L"join")));
    EXPECT(registry.Add(Entry{.name = L"p2.in", .shape = Shape::OnOff, .get = [] { return Value{true}; }}));
    EXPECT(registry.Entries().size() == 2);
    EXPECT(registry.Entries()[0].name == L"join" && registry.Entries()[1].name == L"p2.in");
    EXPECT(registry.Find(L"join") == &registry.Entries()[0]);
    EXPECT(registry.Find(L"p2.in") == &registry.Entries()[1]);
    EXPECT(registry.Find(L"leave") == nullptr);
    EXPECT(registry.Find(L"JOIN") == nullptr);
    EXPECT(registry.Find(L"") == nullptr);
    EXPECT(registry.Rejected().empty());
}

void TestRefusals()
{
    std::puts("registry: a taken name and every malformed entry are refused and remembered");
    Registry registry;
    EXPECT(registry.Add(Action(L"join")));
    EXPECT(!registry.Add(Action(L"join")));
    EXPECT(!registry.Add(Action(L"")));
    EXPECT(!registry.Add(Action(L"two words")));
    EXPECT(!registry.Add(Action(L"tab\tname")));
    EXPECT(!registry.Add(Entry{.name = L"nothing"}));
    EXPECT(!registry.Add(Entry{.name = L"no-words", .shape = Shape::Word, .set = Done}));
    EXPECT(!registry.Add(Entry{.name = L"upside-down", .shape = Shape::Integer, .min = 5, .max = 1, .set = Done}));
    EXPECT(!registry.Add(
        Entry{.name = L"upside-down-float", .shape = Shape::Float, .minFloat = 2.0, .maxFloat = 1.0, .set = Done}));
    EXPECT(!registry.Add(Entry{.name = L"negative-mask", .shape = Shape::Hex, .max = -1, .set = Done}));
    // A NaN bound compares false both ways, so the parser would take every finite number; a Hex floor would never be
    // checked (the parser tests a mask against its maximum only).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT(!registry.Add(
        Entry{.name = L"nan-floor", .shape = Shape::Float, .minFloat = nan, .maxFloat = 1.0, .set = Done}));
    EXPECT(!registry.Add(Entry{.name = L"nan-ceiling", .shape = Shape::Float, .maxFloat = nan, .set = Done}));
    EXPECT(!registry.Add(Entry{.name = L"mask-floor", .shape = Shape::Hex, .min = 0x10, .max = 0xFF, .set = Done}));
    EXPECT(registry.Entries().size() == 1);
    EXPECT(registry.Find(L"join")->set != nullptr);

    const auto& rejected = registry.Rejected();
    EXPECT(rejected.size() == 12);
    EXPECT(rejected[0] == L"join" && rejected[1] == L"(unnamed)" && rejected[2] == L"two words");
    EXPECT(rejected[4] == L"nothing" && rejected[8] == L"negative-mask");
    EXPECT(rejected[9] == L"nan-floor" && rejected[10] == L"nan-ceiling" && rejected[11] == L"mask-floor");

    std::puts("registry: a read-only value and a range of one number are well formed");
    EXPECT(registry.Add(Entry{.name = L"tick", .shape = Shape::Integer, .max = 100, .get = [] { return Value{}; }}));
    EXPECT(registry.Add(Entry{.name = L"one", .shape = Shape::Integer, .min = 3, .max = 3, .set = Done}));
}

void TestNoneAndOnOff()
{
    std::puts("registry: an action takes no argument; a switch takes on or off only");
    const Entry action = Action(L"join");
    EXPECT(ParseValue(action, L"").value.has_value());
    EXPECT(Refused(action, L"now"));
    EXPECT(ParseValue(action, L"now").error == L"join takes no argument");

    const Entry onOff{.name = L"bars", .shape = Shape::OnOff, .set = Done};
    EXPECT(std::get<bool>(*ParseValue(onOff, L"on").value));
    EXPECT(!std::get<bool>(*ParseValue(onOff, L"off").value));
    for (const wchar_t* text : {L"", L"ON", L"yes", L"1", L"on now"})
    {
        EXPECT(Refused(onOff, text));
    }
    EXPECT(RoundTrip(onOff, L"on") == L"on" && RoundTrip(onOff, L"off") == L"off");
}

void TestInteger()
{
    std::puts("registry: an integer within its range, negatives included; anything else refused");
    const Entry offset{.name = L"framing-offset", .shape = Shape::Integer, .min = -2, .max = 5, .set = Done};
    EXPECT(std::get<std::int64_t>(*ParseValue(offset, L"-2").value) == -2);
    EXPECT(std::get<std::int64_t>(*ParseValue(offset, L"0").value) == 0);
    EXPECT(std::get<std::int64_t>(*ParseValue(offset, L"5").value) == 5);
    for (const wchar_t* text : {L"", L"-3", L"6", L"+1", L" 1", L"1 ", L"1.0", L"1e1", L"0x1", L"-", L"--1", L"one"})
    {
        EXPECT(Refused(offset, text));
    }
    EXPECT(ParseValue(offset, L"6").error == L"framing-offset takes a whole number from -2 to 5");
    EXPECT(RoundTrip(offset, L"-2") == L"-2" && RoundTrip(offset, L"5") == L"5");
    EXPECT(RoundTrip(offset, L"005") == L"5");
    EXPECT(RoundTrip(offset, L"-0") == L"0");

    std::puts("registry: a number too large for 64 bits or outside ASCII is refused, not wrapped");
    const Entry wide{.name = L"wide", .shape = Shape::Integer, .min = INT64_MIN, .max = INT64_MAX, .set = Done};
    EXPECT(RoundTrip(wide, L"9223372036854775807") == L"9223372036854775807");
    EXPECT(RoundTrip(wide, L"-9223372036854775808") == L"-9223372036854775808");
    EXPECT(Refused(wide, L"9223372036854775808"));
    EXPECT(Refused(wide, L"99999999999999999999"));
    EXPECT(Refused(wide, L"\x0661"));  // ARABIC-INDIC DIGIT ONE
}

void TestHex()
{
    std::puts("registry: a hexadecimal mask up to its maximum, without a sign or prefix");
    const Entry mask{.name = L"showflags", .shape = Shape::Hex, .max = 0xFFFF, .set = Done};
    EXPECT(std::get<std::int64_t>(*ParseValue(mask, L"100A").value) == 0x100A);
    EXPECT(std::get<std::int64_t>(*ParseValue(mask, L"ff").value) == 0xFF);
    EXPECT(std::get<std::int64_t>(*ParseValue(mask, L"0").value) == 0);
    for (const wchar_t* text : {L"", L"10000", L"-1", L"-0", L"0x10", L"G", L"+1"})
    {
        EXPECT(Refused(mask, text));
    }
    EXPECT(ParseValue(mask, L"10000").error == L"showflags takes a hexadecimal mask up to FFFF");
    EXPECT(RoundTrip(mask, L"ff") == L"FF");

    std::puts("registry: a mask too large for 64 bits is refused; a negative value read back writes with its sign");
    const Entry wide{.name = L"wide-mask", .shape = Shape::Hex, .max = INT64_MAX, .set = Done};
    EXPECT(RoundTrip(wide, L"7FFFFFFFFFFFFFFF") == L"7FFFFFFFFFFFFFFF");
    EXPECT(Refused(wide, L"10000000000000000"));
    EXPECT(Format(wide, Value{std::int64_t{-1}}) == L"-1");  // a module's get never returns one
}

void TestWordFloatText()
{
    std::puts("registry: a word from its list, exactly as written");
    const Entry level{.name = L"single-view", .shape = Shape::Word, .words = {L"auto", L"on", L"off"}, .set = Done};
    EXPECT(std::get<std::wstring>(*ParseValue(level, L"auto").value) == L"auto");
    EXPECT(RoundTrip(level, L"off") == L"off");
    for (const wchar_t* text : {L"", L"Auto", L"maybe", L"on off"})
    {
        EXPECT(Refused(level, text));
    }
    EXPECT(ParseValue(level, L"maybe").error == L"single-view takes auto|on|off");

    std::puts("registry: a finite number within its range");
    const Entry blend{.name = L"split-fov", .shape = Shape::Float, .minFloat = 0.0, .maxFloat = 1.0, .set = Done};
    EXPECT(std::get<double>(*ParseValue(blend, L"0.5").value) == 0.5);
    EXPECT(std::get<double>(*ParseValue(blend, L"1").value) == 1.0);
    EXPECT(std::get<double>(*ParseValue(blend, L"0").value) == 0.0);
    for (const wchar_t* text : {L"", L"-0.1", L"1.01", L"nan", L"inf", L"0.5x", L" 0.5", L"+0.5", L"half"})
    {
        EXPECT(Refused(blend, text));
    }
    EXPECT(RoundTrip(blend, L"0.5") == L"0.5" && RoundTrip(blend, L"0.25") == L"0.25");

    std::puts("registry: a float's sign, exponent and bare point; an exponent out of double's range refused");
    const Entry signedReal{.name = L"tilt", .shape = Shape::Float, .minFloat = -10.0, .maxFloat = 10.0, .set = Done};
    EXPECT(RoundTrip(signedReal, L"-0.5") == L"-0.5");
    EXPECT(RoundTrip(signedReal, L"1e1") == L"10");
    EXPECT(RoundTrip(signedReal, L".5") == L"0.5");
    EXPECT(RoundTrip(signedReal, L"5.") == L"5");
    EXPECT(Refused(signedReal, L"1e999"));
    EXPECT(Refused(signedReal, L"-1e999"));

    std::puts("registry: text takes the rest of the line, but not nothing");
    const Entry note{.name = L"console", .shape = Shape::Text, .set = Done};
    EXPECT(RoundTrip(note, L"stat fps") == L"stat fps");
    EXPECT(Refused(note, L""));
    EXPECT(ParseValue(note, L"").error == L"console needs text");
}

void TestNamedValues()
{
    std::puts("registry: a number shape takes its named values as text, and says so when nothing fits");
    const Entry blend{.name = L"split-fov", .shape = Shape::Float, .minFloat = 0.0, .maxFloat = 1.0,
                      .words = {L"native"}, .set = Done};
    EXPECT(std::get<std::wstring>(*ParseValue(blend, L"native").value) == L"native");
    EXPECT(std::get<double>(*ParseValue(blend, L"0.5").value) == 0.5);
    EXPECT(RoundTrip(blend, L"native") == L"native");
    EXPECT(Refused(blend, L"Native") && Refused(blend, L"2"));
    EXPECT(ParseValue(blend, L"2").error == L"split-fov takes a number from 0 to 1, or native");

    const Entry degrees{.name = L"stream-fov-p1", .shape = Shape::Integer, .min = 1, .max = 170,
                        .words = {L"auto"}, .set = Done};
    EXPECT(std::get<std::wstring>(*ParseValue(degrees, L"auto").value) == L"auto");
    EXPECT(std::get<std::int64_t>(*ParseValue(degrees, L"90").value) == 90);
    EXPECT(ParseValue(degrees, L"0").error == L"stream-fov-p1 takes a whole number from 1 to 170, or auto");

    std::puts("registry: ReadWhole and ReadReal, the readers the script shares");
    EXPECT(lss::control::ReadWhole(L"36000", 10) == 36000 && !lss::control::ReadWhole(L"+1", 10));
    EXPECT(lss::control::ReadWhole(L"-5", 10) == -5 && !lss::control::ReadWhole(L"-5", 16));
    EXPECT(lss::control::ReadReal(L"-0.25") == -0.25 && !lss::control::ReadReal(L"inf"));

    std::puts("registry: a float setting reads back as the decimal it was set with");
    const Entry fraction{.name = L"fraction", .shape = Shape::Float, .minFloat = 0.0, .maxFloat = 1.0, .set = Done};
    EXPECT(lss::control::FloatReading(0.3f) == 0.3);
    EXPECT(Format(fraction, Value{lss::control::FloatReading(0.3f)}) == L"0.3");
    EXPECT(Format(fraction, Value{static_cast<double>(0.3f)}) != L"0.3");  // what the plain widening wrote
    EXPECT(lss::control::FloatReading(0.5f) == 0.5 && lss::control::FloatReading(0.7f) == 0.7);
    EXPECT(lss::control::FloatReading(-1.0f) == -1.0 && lss::control::FloatReading(0.0f) == 0.0);
}

void TestFormatEmpty()
{
    std::puts("registry: an empty value writes as nothing");
    EXPECT(Format(Action(L"join"), Value{}).empty());
}

void TestRegisterOption()
{
    std::puts("registry: an option's row becomes a command: on|off an OnOff, other words a Word, the default in the help");
    using lss::platform::settings::kMultiMonitor;
    using lss::platform::settings::kTwoPlayerSplit;
    std::wstring held = L"off";
    lss::control::RegisterOption(
        kMultiMonitor,
        [&held](const std::wstring& word) {
            held = word;
            return Outcome{};
        },
        [&held] { return held; });
    const Entry* monitors = lss::control::Global().Find(L"multi-monitor");
    EXPECT(monitors != nullptr);
    if (monitors != nullptr)
    {
        EXPECT(monitors->shape == Shape::OnOff && monitors->words.empty());
        EXPECT(monitors->help.ends_with(L"(default: off)"));
        EXPECT(monitors->set(Value{true}).ok && held == L"on");
        EXPECT(std::get<bool>(monitors->get()));
        EXPECT(monitors->set(Value{false}).ok && held == L"off");
        EXPECT(!std::get<bool>(monitors->get()));
    }
    std::wstring split = L"horizontal";
    lss::control::RegisterOption(
        kTwoPlayerSplit,
        [&split](const std::wstring& word) {
            split = word;
            return Outcome{true, word};
        },
        [&split] { return split; });
    const Entry* layout = lss::control::Global().Find(L"two-player-split");
    EXPECT(layout != nullptr);
    if (layout != nullptr)
    {
        EXPECT(layout->shape == Shape::Word && layout->words.size() == 2);
        EXPECT(layout->words[0] == L"vertical" && layout->words[1] == L"horizontal");
        EXPECT(layout->help.ends_with(L"(default: horizontal)"));
        EXPECT(RoundTrip(*layout, L"vertical") == L"vertical");
        EXPECT(Refused(*layout, L"sideways") && Refused(*layout, L"Vertical"));
        EXPECT(layout->set(Value{std::wstring(L"vertical")}).ok && split == L"vertical");
        EXPECT(std::get<std::wstring>(layout->get()) == L"vertical");
    }
}

}  // namespace

int main()
{
    TestAddAndFind();
    TestRefusals();
    TestNoneAndOnOff();
    TestInteger();
    TestHex();
    TestWordFloatText();
    TestNamedValues();
    TestFormatEmpty();
    TestRegisterOption();

    return lss::test::Report();
}
