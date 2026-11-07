#include "platform/command_registry.h"

#include "platform/settings.h"

#include <charconv>
#include <cmath>
#include <format>
#include <utility>

namespace lss::control {

namespace {

/// The word as ASCII, or nothing if any character is outside it (a number never needs more).
std::optional<std::string> Ascii(std::wstring_view word)
{
    std::string out;
    out.reserve(word.size());
    for (const wchar_t c : word)
    {
        if (c <= 0 || c > 0x7F)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::wstring Alternatives(const std::vector<std::wstring>& words)
{
    std::wstring out;
    for (const auto& word : words)
    {
        out += out.empty() ? word : L"|" + word;
    }
    return out;
}

/// The entry's named value the text spells (a number shape's `native`, `auto`), if any.
std::optional<Value> NamedValue(const Entry& entry, std::wstring_view text)
{
    for (const auto& word : entry.words)
    {
        if (text == word)
        {
            return Value{word};
        }
    }
    return std::nullopt;
}

/// ", or a|b" after a number shape's range when it has named values.
std::wstring OrNamed(const Entry& entry)
{
    return entry.words.empty() ? std::wstring{} : L", or " + Alternatives(entry.words);
}

}  // namespace

// from_chars takes no locale, leading space or '+'; it would take a '-' in any base for a signed type, so a mask's
// '-' is refused here.
std::optional<std::int64_t> ReadWhole(std::wstring_view word, int base)
{
    const auto text = Ascii(word);
    if (!text || text->empty() || (base != 10 && text->front() == '-'))
    {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value, base);
    if (error != std::errc{} || end != text->data() + text->size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<double> ReadReal(std::wstring_view word)
{
    const auto text = Ascii(word);
    if (!text || text->empty())
    {
        return std::nullopt;
    }
    double     value = 0.0;
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size() || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

double FloatReading(float value) noexcept
{
    // to_chars without a precision gives the shortest text that reads back as the same float; read as a double, that
    // text is the value a script would have written.
    char       text[64];
    const auto [end, error] = std::to_chars(text, text + sizeof(text), value);
    double     out          = static_cast<double>(value);
    if (error == std::errc{})
    {
        std::from_chars(text, end, out);
    }
    return out;
}

std::wstring Format(const Entry& entry, const Value& value)
{
    if (const auto* on = std::get_if<bool>(&value))
    {
        return *on ? L"on" : L"off";
    }
    if (const auto* number = std::get_if<std::int64_t>(&value))
    {
        return entry.shape == Shape::Hex ? std::format(L"{:X}", *number) : std::to_wstring(*number);
    }
    if (const auto* real = std::get_if<double>(&value))
    {
        return std::format(L"{}", *real);
    }
    if (const auto* text = std::get_if<std::wstring>(&value))
    {
        return *text;
    }
    return {};
}

ParsedValue ParseValue(const Entry& entry, std::wstring_view text)
{
    switch (entry.shape)
    {
    case Shape::None:
        return text.empty() ? ParsedValue{Value{}, {}} : ParsedValue{std::nullopt, entry.name + L" takes no argument"};
    case Shape::OnOff:
        if (text == L"on" || text == L"off")
        {
            return {Value{text == L"on"}, {}};
        }
        return {std::nullopt, entry.name + L" takes on or off"};
    case Shape::Integer:
    {
        if (auto named = NamedValue(entry, text))
        {
            return {std::move(named), {}};
        }
        const auto value = ReadWhole(text, 10);
        if (!value || *value < entry.min || *value > entry.max)
        {
            return {std::nullopt, std::format(L"{} takes a whole number from {} to {}{}", entry.name, entry.min,
                                              entry.max, OrNamed(entry))};
        }
        return {Value{*value}, {}};
    }
    case Shape::Hex:
    {
        const auto value = ReadWhole(text, 16);
        if (!value || *value > entry.max)
        {
            return {std::nullopt, std::format(L"{} takes a hexadecimal mask up to {:X}", entry.name, entry.max)};
        }
        return {Value{*value}, {}};
    }
    case Shape::Word:
        if (auto named = NamedValue(entry, text))
        {
            return {std::move(named), {}};
        }
        return {std::nullopt, std::format(L"{} takes {}", entry.name, Alternatives(entry.words))};
    case Shape::Float:
    {
        if (auto named = NamedValue(entry, text))
        {
            return {std::move(named), {}};
        }
        const auto value = ReadReal(text);
        if (!value || *value < entry.minFloat || *value > entry.maxFloat)
        {
            return {std::nullopt, std::format(L"{} takes a number from {} to {}{}", entry.name, entry.minFloat,
                                              entry.maxFloat, OrNamed(entry))};
        }
        return {Value{*value}, {}};
    }
    case Shape::Text:
        if (text.empty())
        {
            return {std::nullopt, entry.name + L" needs text"};
        }
        return {Value{std::wstring(text)}, {}};
    }
    return {std::nullopt, L"unknown shape"};
}

bool Registry::Add(Entry entry)
{
    const bool malformed = entry.name.empty() || entry.name.find_first_of(L" \t\r\n") != std::wstring::npos ||
                           (!entry.set && !entry.get) || (entry.shape == Shape::Word && entry.words.empty()) ||
                           (entry.shape == Shape::Integer && entry.max < entry.min) ||
                           (entry.shape == Shape::Float && !(entry.minFloat <= entry.maxFloat)) ||  // NaN too
                           (entry.shape == Shape::Hex && (entry.min != 0 || entry.max < 0));
    if (malformed || Find(entry.name) != nullptr)
    {
        rejected_.push_back(entry.name.empty() ? L"(unnamed)" : entry.name);
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

const Entry* Registry::Find(std::wstring_view name) const noexcept
{
    for (const auto& entry : entries_)
    {
        if (entry.name == name)
        {
            return &entry;
        }
    }
    return nullptr;
}

Registry& Global() noexcept
{
    static Registry registry;
    return registry;
}

void RegisterOption(const platform::settings::Option& option, std::function<Outcome(const std::wstring& word)> set,
                    std::function<std::wstring()> get)
{
    if (option.words.empty())
    {
        Global().Add(Entry{.name = option.key});  // refused and remembered: the console logs it as a rejected name
        return;
    }
    // In either order: the page lists an option's words as the table does.
    const bool onOff = option.words.size() == 2 &&
                       ((std::wstring_view(option.words[0]) == L"on" && std::wstring_view(option.words[1]) == L"off") ||
                        (std::wstring_view(option.words[0]) == L"off" && std::wstring_view(option.words[1]) == L"on"));
    Entry entry{.name  = option.key,
                .shape = onOff ? Shape::OnOff : Shape::Word,
                .help  = std::wstring(option.help) + L" (default: " + option.fallback + L")"};
    if (!onOff)
    {
        entry.words.assign(option.words.begin(), option.words.end());
    }
    entry.set = [set = std::move(set), onOff](const Value& v) {
        return set(onOff ? std::wstring(std::get<bool>(v) ? L"on" : L"off") : std::get<std::wstring>(v));
    };
    entry.get = [get = std::move(get), onOff]() -> Value {
        const std::wstring word = get();
        return onOff ? Value{word == L"on"} : Value{word};
    };
    Register(std::move(entry));
}

}  // namespace lss::control
