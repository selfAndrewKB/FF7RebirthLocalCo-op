// Host-side tests for the control language (control/script.h) against a registry of its own, run without the game.
// Covered beyond the language's basics: the named values of a number shape, and hold with three players.

#include "control/script.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using namespace lss::control;

Outcome Done(const Value&)
{
    return {};
}

Registry FakeRegistry()
{
    Registry registry;
    registry.Add({.name = L"join", .help = L"player 2 joins", .set = Done});
    registry.Add(
        {.name = L"bars", .shape = Shape::OnOff, .help = L"bars", .set = Done, .get = [] { return Value{true}; }});
    registry.Add({.name = L"showflags", .shape = Shape::Hex, .max = 0xFFFF, .help = L"forced flags", .set = Done,
                  .get = [] { return Value{std::int64_t{0x100A}}; }});
    registry.Add({.name = L"log-level", .shape = Shape::Word, .words = {L"info", L"debug", L"trace"},
                  .help = L"the log level", .set = Done, .get = [] { return Value{std::wstring(L"info")}; }});
    registry.Add({.name = L"split-fov", .shape = Shape::Float, .minFloat = 0.0, .maxFloat = 1.0, .words = {L"native"},
                  .help = L"a blend", .set = Done, .get = [] { return Value{0.5}; }});
    registry.Add({.name = L"stream-fov-p1", .shape = Shape::Integer, .min = 1, .max = 170, .words = {L"auto"},
                  .help = L"degrees", .set = Done, .get = [] { return Value{std::wstring(L"auto")}; }});
    registry.Add({.name = L"console", .shape = Shape::Text, .help = L"an engine console command", .set = Done});
    registry.Add({.name = L"tick", .shape = Shape::Integer, .max = INT64_MAX, .help = L"the engine tick",
                  .get = [] { return Value{std::int64_t{7}}; }});
    registry.Add({.name = L"framing", .shape = Shape::Integer, .min = -2, .max = 5, .help = L"steps", .set = Done,
                  .get = [] { return Value{std::int64_t{0}}; }});
    registry.Add({.name = L"p2.state", .shape = Shape::Word, .words = {L"out", L"playing"}, .help = L"player 2",
                  .get = [] { return Value{std::wstring(L"out")}; }});
    return registry;
}

bool Rejected(const Registry& registry, const wchar_t* line)
{
    const auto parsed = ParseLine(registry, line);
    return !parsed.command && !parsed.error.empty();
}

std::wstring RoundTrip(const Registry& registry, const wchar_t* line)
{
    const auto parsed = ParseLine(registry, line);
    return parsed.command ? Describe(registry, *parsed.command) : L"<" + parsed.error + L">";
}

void TestBlankAndUnknown()
{
    std::puts("script: blank lines and comments are skipped; unknown words and stray arguments rejected");
    const auto registry = FakeRegistry();
    for (const wchar_t* line : {L"", L"   ", L"# a comment", L"  # indented", L"\r"})
    {
        const auto parsed = ParseLine(registry, line);
        EXPECT(!parsed.command && parsed.error.empty());
    }
    EXPECT(Rejected(registry, L"frobnicate"));
    EXPECT(Rejected(registry, L"JOIN"));
    EXPECT(Rejected(registry, L"status now"));
    EXPECT(Rejected(registry, L"abort 1"));
    EXPECT(ParseLine(registry, L"frobnicate").error == L"unknown command 'frobnicate' (try help)");
}

void TestEntries()
{
    std::puts("script: a registered command takes one argument in its shape; surrounding space and a CR ignored");
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"join") == L"join");
    EXPECT(RoundTrip(registry, L"join\r") == L"join");
    EXPECT(Rejected(registry, L"join now"));
    EXPECT(RoundTrip(registry, L"bars on") == L"bars on");
    EXPECT(RoundTrip(registry, L"  bars   off ") == L"bars off");
    EXPECT(Rejected(registry, L"bars maybe"));
    EXPECT(Rejected(registry, L"bars"));
    EXPECT(Rejected(registry, L"bars on on"));
    EXPECT(RoundTrip(registry, L"showflags 100a") == L"showflags 100A");
    EXPECT(Rejected(registry, L"showflags 10000"));
    EXPECT(RoundTrip(registry, L"log-level debug") == L"log-level debug");
    EXPECT(Rejected(registry, L"log-level loud"));
    EXPECT(RoundTrip(registry, L"console r.SSR.Quality 0 ") == L"console r.SSR.Quality 0");
    EXPECT(Rejected(registry, L"console"));
    EXPECT(RoundTrip(registry, L"framing -2") == L"framing -2");
    EXPECT(Rejected(registry, L"framing -3"));
    EXPECT(Rejected(registry, L"tick 5"));  // read only
    EXPECT(ParseLine(registry, L"tick 5").error == L"tick is only read (get tick)");
}

void TestNamedValues()
{
    std::puts("script: a number shape also takes its named values (split-fov native, stream-fov-p1 auto)");
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"split-fov 0.5") == L"split-fov 0.5");
    EXPECT(RoundTrip(registry, L"split-fov 1") == L"split-fov 1");
    EXPECT(RoundTrip(registry, L"split-fov native") == L"split-fov native");
    EXPECT(std::holds_alternative<std::wstring>(ParseLine(registry, L"split-fov native").command->value));
    EXPECT(Rejected(registry, L"split-fov 1.5"));
    EXPECT(Rejected(registry, L"split-fov Native"));
    EXPECT(RoundTrip(registry, L"stream-fov-p1 90") == L"stream-fov-p1 90");
    EXPECT(RoundTrip(registry, L"stream-fov-p1 auto") == L"stream-fov-p1 auto");
    EXPECT(Rejected(registry, L"stream-fov-p1 0"));
    EXPECT(Rejected(registry, L"stream-fov-p1 171"));
    EXPECT(ParseLine(registry, L"stream-fov-p1 0").error ==
           L"stream-fov-p1 takes a whole number from 1 to 170, or auto");
}

void TestQueries()
{
    std::puts("script: get, status, help and note");
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"get tick") == L"get tick");
    EXPECT(RoundTrip(registry, L"get bars") == L"get bars");
    EXPECT(Rejected(registry, L"get join"));  // an action has no value
    EXPECT(Rejected(registry, L"get nothing"));
    EXPECT(Rejected(registry, L"get"));
    EXPECT(RoundTrip(registry, L"status") == L"status");
    EXPECT(RoundTrip(registry, L"help") == L"help");
    EXPECT(RoundTrip(registry, L"help split-fov") == L"help split-fov");
    EXPECT(Rejected(registry, L"help nothing"));
    EXPECT(RoundTrip(registry, L"note hello   world") == L"note hello   world");
    EXPECT(Rejected(registry, L"note"));
}

void TestWaits()
{
    std::puts("script: wait and wait-until, the ordered ops for numbers only");
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"wait 120") == L"wait 120");
    EXPECT(Rejected(registry, L"wait 0"));
    EXPECT(Rejected(registry, L"wait 36001"));
    EXPECT(Rejected(registry, L"wait"));
    EXPECT(Rejected(registry, L"wait 1 2"));
    EXPECT(Rejected(registry, L"wait +5"));
    EXPECT(RoundTrip(registry, L"wait-until p2.state == playing 1800") == L"wait-until p2.state == playing 1800");
    EXPECT(RoundTrip(registry, L"wait-until bars == on") == L"wait-until bars == on 600");
    EXPECT(RoundTrip(registry, L"wait-until tick >= 100") == L"wait-until tick >= 100 600");
    EXPECT(RoundTrip(registry, L"wait-until framing < 0 900") == L"wait-until framing < 0 900");
    EXPECT(RoundTrip(registry, L"wait-until log-level != trace") == L"wait-until log-level != trace 600");
    EXPECT(RoundTrip(registry, L"wait-until split-fov == native") == L"wait-until split-fov == native 600");
    EXPECT(RoundTrip(registry, L"wait-until split-fov > 0.25") == L"wait-until split-fov > 0.25 600");
    EXPECT(Rejected(registry, L"wait-until split-fov > native"));  // a named value is not ordered
    EXPECT(Rejected(registry, L"wait-until bars < on"));           // on/off is not ordered
    EXPECT(Rejected(registry, L"wait-until log-level > info"));
    EXPECT(Rejected(registry, L"wait-until p2.state == lost"));
    EXPECT(Rejected(registry, L"wait-until join == 1"));  // no value to read
    EXPECT(Rejected(registry, L"wait-until tick ~ 1"));
    EXPECT(Rejected(registry, L"wait-until tick == x"));
    EXPECT(Rejected(registry, L"wait-until tick == 1 0"));
    EXPECT(Rejected(registry, L"wait-until tick == 1 2 3"));
    EXPECT(Rejected(registry, L"wait-until playing 1800"));  // the form before the port
}

void TestHold()
{
    std::puts("script: hold, for player 1, 2 or 3");
    const auto registry = FakeRegistry();
    EXPECT(RoundTrip(registry, L"hold 2 stick 0 1 0 0 120") == L"hold 2 stick 0 1 0 0 120");
    EXPECT(RoundTrip(registry, L"hold 1 stick -1 0.5 0 -0.25 1") == L"hold 1 stick -1 0.5 0 -0.25 1");
    EXPECT(RoundTrip(registry, L"hold 3 stick 0 -1 0 0 60") == L"hold 3 stick 0 -1 0 0 60");
    EXPECT(Rejected(registry, L"hold 4 stick 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold 12 stick 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold x stick 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold 0 stick 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 button 0 0 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 2 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 nan 0 0 1"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 0 0 0 0"));
    EXPECT(Rejected(registry, L"hold 2 stick 0 0 0 0"));
    EXPECT(Rejected(registry, L"stick 2 0 1 0 0 180"));  // the form before the port
}

void TestShapeText()
{
    std::puts("script: help's shapes, the named values after the range");
    const auto registry = FakeRegistry();
    EXPECT(ShapeText(*registry.Find(L"join")).empty());
    EXPECT(ShapeText(*registry.Find(L"bars")) == L"on|off");
    EXPECT(ShapeText(*registry.Find(L"showflags")) == L"<hex up to FFFF>");
    EXPECT(ShapeText(*registry.Find(L"log-level")) == L"info|debug|trace");
    EXPECT(ShapeText(*registry.Find(L"framing")) == L"<-2..5>");
    EXPECT(ShapeText(*registry.Find(L"split-fov")) == L"<0..1>|native");
    EXPECT(ShapeText(*registry.Find(L"stream-fov-p1")) == L"<1..170>|auto");
    EXPECT(ShapeText(*registry.Find(L"console")) == L"<text>");
}

}  // namespace

int main()
{
    TestBlankAndUnknown();
    TestEntries();
    TestNamedValues();
    TestQueries();
    TestWaits();
    TestHold();
    TestShapeText();

    return lss::test::Report();
}
