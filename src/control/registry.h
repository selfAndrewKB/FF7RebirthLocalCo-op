#pragma once

// The command registry: every switch, action and readable value the control console knows, as the modules register
// them. One entry gives a command its name, its argument's shape, what it does (set or act), what it reads back (get)
// and one line of help. The parser checks a line against the shape, and `help`, `get`, `status` and `wait-until` are
// generated from the table, so a module adds a command with one call beside the code it drives, and nothing else in
// the mod names it.
//
//   control::Register({.name = L"bars", .shape = Shape::OnOff, .help = L"...",
//                      .set = [](const Value& v) { ...; return Outcome{}; },
//                      .get = [] { return Value{BarsOn()}; }});
//
// Entries are added on the game thread before the console opens, and read on the game thread. The registry has no
// engine dependency, so the parser and the runner are tested on the host with entries of their own
// (tests/control_registry_test.cpp). The control route is a development tool: the registrations sit behind the dev
// switch (platform/dev_switch.h), so a player's build carries no table.
//
// Taken from Remake's control/registry (2026-10-23). Changed: an Integer argument may be negative (Rebirth's
// framing offsets go below zero); the numbers are read with std::from_chars on an ASCII copy, so no locale, '+',
// leading space or non-ASCII digit of the C library's conversions slips through; Add also refuses NaN float bounds
// and a Hex floor (the parser checks a Hex mask against its maximum only).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lss::control {

/// What a command did; `detail` says why when it failed, and may add a fact when it succeeded.
struct Outcome
{
    bool         ok = true;
    std::wstring detail;
};

/// The shape of a command's one argument, which the parser checks before anything runs.
enum class Shape : std::uint8_t
{
    None,     // no argument: an action (join), or a value that is only read
    OnOff,    // on | off
    Integer,  // a whole number in [min, max], a leading '-' allowed
    Hex,      // a hexadecimal mask, at most max (e.g. 100A)
    Word,     // one of `words`
    Float,    // a number in [minFloat, maxFloat]
    Text,     // the rest of the line (console, note)
};

/// An argument or a read value: a bool (OnOff), an integer (Integer, Hex), a double (Float) or text (Word, Text).
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::wstring>;

struct Entry
{
    std::wstring              name;
    Shape                     shape    = Shape::None;
    std::int64_t              min      = 0;    // Integer; Hex: must stay 0
    std::int64_t              max      = 0;    // Integer, Hex
    double                    minFloat = 0.0;  // Float
    double                    maxFloat = 0.0;  // Float
    std::vector<std::wstring> words;           // Word
    std::wstring              help;

    /// Runs the command with its argument (an empty Value for Shape::None). Absent for a read-only value.
    std::function<Outcome(const Value&)> set;
    /// The current value, in the entry's shape (an OnOff reads as a bool, a Word as text). Absent for an action.
    std::function<Value()> get;
};

/// The value as a script writes it: on/off, a decimal, a hex mask, or the text itself.
std::wstring Format(const Entry& entry, const Value& value);

/// Checks one argument word (or the rest of the line, for Text) against the entry's shape; the message says what the
/// shape wants when it does not fit.
struct ParsedValue
{
    std::optional<Value> value;
    std::wstring         error;
};
ParsedValue ParseValue(const Entry& entry, std::wstring_view text);

/// The table. A class so tests build their own; the mod uses the one in Global().
class Registry
{
public:
    /// Adds an entry; false (and no change) if the name is taken or the entry is malformed (an empty name or one with
    /// white space, a Word shape with no words, a range with max below min or a NaN bound, a Hex entry with a floor or
    /// a negative maximum, neither set nor get).
    bool Add(Entry entry);

    /// The entry, or null. The pointer lasts until the next Add (the entries live in a vector), which is why every Add
    /// comes before the console opens.
    const Entry* Find(std::wstring_view name) const noexcept;

    /// Every entry, in registration order.
    const std::vector<Entry>& Entries() const noexcept { return entries_; }

    /// The names Add refused, for the console to log when it opens (the registry itself cannot log: it has no engine
    /// dependency, so the host tests can build it).
    const std::vector<std::wstring>& Rejected() const noexcept { return rejected_; }

private:
    std::vector<Entry>        entries_;
    std::vector<std::wstring> rejected_;
};

/// The mod's registry.
Registry& Global() noexcept;

/// `Global().Add(entry)`; a refusal is kept in Rejected().
inline void Register(Entry entry) { Global().Add(std::move(entry)); }

}  // namespace lss::control
