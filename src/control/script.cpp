#include "control/script.h"

#include <cmath>
#include <cwchar>
#include <format>
#include <vector>

namespace lss::control {

namespace {

constexpr std::wstring_view kSpace = L" \t\r\n";

std::wstring_view Trim(std::wstring_view text)
{
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

std::vector<std::wstring_view> Words(std::wstring_view text)
{
    std::vector<std::wstring_view> words;
    while (!(text = Trim(text)).empty())
    {
        const auto end = text.find_first_of(kSpace);
        words.push_back(text.substr(0, end));
        text = end == std::wstring_view::npos ? std::wstring_view{} : text.substr(end);
    }
    return words;
}

/// The text after the first word, trimmed.
std::wstring_view Rest(std::wstring_view line)
{
    line            = Trim(line);
    const auto end  = line.find_first_of(kSpace);
    return end == std::wstring_view::npos ? std::wstring_view{} : Trim(line.substr(end));
}

std::optional<std::uint32_t> ParseTicks(std::wstring_view word)
{
    const std::wstring text(word);
    wchar_t*           end   = nullptr;
    const auto         value = std::wcstoul(text.c_str(), &end, 10);
    if (text.empty() || text[0] == L'-' || end != text.c_str() + text.size() || value == 0 || value > kMaxTicks)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

std::optional<float> ParseAxis(std::wstring_view word)
{
    const std::wstring text(word);
    wchar_t*           end   = nullptr;
    const float        value = std::wcstof(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) || value < -1.0f || value > 1.0f)
    {
        return std::nullopt;
    }
    return value;
}

ParsedLine Error(std::wstring message)
{
    return {std::nullopt, std::move(message)};
}

ParsedLine Parsed(Command command)
{
    return {std::move(command), {}};
}

ParsedLine ParseSwitch(CommandKind kind, const std::vector<std::wstring_view>& words, std::wstring_view onWord,
                       std::wstring_view offWord)
{
    if (words.size() != 2 || (words[1] != onWord && words[1] != offWord))
    {
        return Error(std::format(L"{} takes {} or {}", words[0], onWord, offWord));
    }
    Command command;
    command.kind = kind;
    command.on   = words[1] == onWord;
    return Parsed(std::move(command));
}

ParsedLine ParseStick(const std::vector<std::wstring_view>& words)
{
    if (words.size() != 7)
    {
        return Error(L"stick takes <player> <lx> <ly> <rx> <ry> <ticks>");
    }
    Command command;
    command.kind = CommandKind::Stick;
    if (words[1] != L"1" && words[1] != L"2")
    {
        return Error(L"stick: the player is 1 or 2");
    }
    command.player = words[1] == L"1" ? 1 : 2;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto axis = ParseAxis(words[2 + i]);
        if (!axis)
        {
            return Error(std::format(L"stick: '{}' is not an axis value from -1 to 1", words[2 + i]));
        }
        command.axes[i] = *axis;
    }
    const auto ticks = ParseTicks(words[6]);
    if (!ticks)
    {
        return Error(std::format(L"stick: the ticks are a whole number from 1 to {}", kMaxTicks));
    }
    command.ticks = *ticks;
    return Parsed(std::move(command));
}

ParsedLine ParseDump(const std::vector<std::wstring_view>& words)
{
    const bool single = words.size() == 2 && (words[1] == L"widgets" || words[1] == L"hud-players");
    const bool slots  = words.size() == 3 && words[1] == L"slots";
    if (!single && !slots)
    {
        return Error(L"dump takes widgets, hud-players, or slots <widget class>");
    }
    Command command;
    command.kind = CommandKind::Dump;
    command.text = single ? std::wstring(words[1]) : std::wstring(words[1]) + L" " + std::wstring(words[2]);
    return Parsed(std::move(command));
}

ParsedLine ParseSplitFov(const std::vector<std::wstring_view>& words)
{
    Command command;
    command.kind = CommandKind::SplitFov;
    if (words.size() == 2 && words[1] == L"native")
    {
        return Parsed(std::move(command));
    }
    const auto blend = words.size() == 2 ? ParseAxis(words[1]) : std::nullopt;
    if (!blend || *blend < 0.0f)
    {
        return Error(L"split-fov takes a blend from 0 to 1, or native");
    }
    command.blend = *blend;
    return Parsed(std::move(command));
}

ParsedLine ParseFramingOffset(const std::vector<std::wstring_view>& words)
{
    Command command;
    command.kind = CommandKind::FramingOffset;
    auto step    = [](std::wstring_view word) -> std::optional<std::int32_t> {
        const bool negative = word.size() == 2 && word[0] == L'-';
        const auto digits   = negative ? word.substr(1) : word;
        if (digits.size() != 1 || digits[0] < L'0' || digits[0] > L'9')
        {
            return std::nullopt;
        }
        const std::int32_t value = negative ? -(digits[0] - L'0') : digits[0] - L'0';
        return value >= -2 && value <= 5 ? std::optional<std::int32_t>(value) : std::nullopt;
    };
    const auto topBottom  = words.size() == 3 ? step(words[1]) : std::nullopt;
    const auto sideBySide = words.size() == 3 ? step(words[2]) : std::nullopt;
    if (!topBottom || !sideBySide)
    {
        return Error(L"framing-offset takes two whole steps from -2 to 5: top/bottom, then side by side");
    }
    command.offsets[0] = *topBottom;
    command.offsets[1] = *sideBySide;
    return Parsed(std::move(command));
}

ParsedLine ParseShoulderMinimum(const std::vector<std::wstring_view>& words)
{
    const auto minimum = words.size() == 2 ? ParseAxis(words[1]) : std::nullopt;
    if (!minimum || *minimum < 0.25f)
    {
        return Error(L"shoulder-scale-min takes a factor from 0.25 to 1");
    }
    Command command;
    command.kind  = CommandKind::ShoulderScaleMin;
    command.blend = *minimum;
    return Parsed(std::move(command));
}

ParsedLine ParseHudTrace(const std::vector<std::wstring_view>& words)
{
    const auto frames = words.size() == 2 ? ParseTicks(words[1]) : std::nullopt;
    if (!frames || *frames > kMaxTraceFrames)
    {
        return Error(std::format(L"hud-trace takes a whole number of frames from 1 to {}", kMaxTraceFrames));
    }
    Command command;
    command.kind  = CommandKind::HudTrace;
    command.ticks = *frames;
    return Parsed(std::move(command));
}

/// "<name> <0..maxMask>": a diagnosis mask.
ParsedLine ParseMask(const std::vector<std::wstring_view>& words, CommandKind kind, unsigned long maxMask)
{
    const std::wstring text  = words.size() == 2 ? std::wstring(words[1]) : std::wstring();
    wchar_t*           end   = nullptr;
    const auto         value = std::wcstoul(text.c_str(), &end, 10);
    if (text.empty() || text[0] == L'-' || end != text.c_str() + text.size() || value > maxMask)
    {
        return Error(std::format(L"{} takes a whole number from 0 to {}", words[0], maxMask));
    }
    Command command;
    command.kind  = kind;
    command.ticks = static_cast<std::uint32_t>(value);
    return Parsed(std::move(command));
}

ParsedLine ParseWait(const std::vector<std::wstring_view>& words)
{
    const auto ticks = words.size() == 2 ? ParseTicks(words[1]) : std::nullopt;
    if (!ticks)
    {
        return Error(std::format(L"wait takes a whole number of ticks from 1 to {}", kMaxTicks));
    }
    Command command;
    command.kind  = CommandKind::Wait;
    command.ticks = *ticks;
    return Parsed(std::move(command));
}

ParsedLine ParseWaitUntil(const std::vector<std::wstring_view>& words)
{
    if ((words.size() != 2 && words.size() != 3) || (words[1] != L"playing" && words[1] != L"out"))
    {
        return Error(L"wait-until takes playing or out, and optionally a timeout in ticks");
    }
    Command command;
    command.kind      = CommandKind::WaitUntil;
    command.condition = words[1] == L"playing" ? WaitCondition::Playing : WaitCondition::Out;
    command.ticks     = kDefaultWaitUntilTicks;
    if (words.size() == 3)
    {
        const auto ticks = ParseTicks(words[2]);
        if (!ticks)
        {
            return Error(std::format(L"wait-until: the timeout is a whole number of ticks from 1 to {}", kMaxTicks));
        }
        command.ticks = *ticks;
    }
    return Parsed(std::move(command));
}

ParsedLine ParseText(CommandKind kind, std::wstring_view name, std::wstring_view line)
{
    const auto text = Rest(line);
    if (text.empty())
    {
        return Error(std::format(L"{} needs some text after it", name));
    }
    Command command;
    command.kind = kind;
    command.text = std::wstring(text);
    return Parsed(std::move(command));
}

ParsedLine ParseSingleView(const std::vector<std::wstring_view>& words)
{
    constexpr std::wstring_view kModes[] = {L"auto", L"on", L"off"};
    for (std::uint32_t mode = 0; words.size() == 2 && mode < std::size(kModes); ++mode)
    {
        if (words[1] == kModes[mode])
        {
            Command command;
            command.kind  = CommandKind::SingleView;
            command.ticks = mode;
            return Parsed(std::move(command));
        }
    }
    return Error(L"single-view takes auto, on or off");
}

ParsedLine ParseStreamViews(const std::vector<std::wstring_view>& words)
{
    constexpr std::wstring_view kModes[] = {L"off", L"p2", L"both"};
    for (std::uint32_t mode = 0; words.size() == 2 && mode < std::size(kModes); ++mode)
    {
        if (words[1] == kModes[mode])
        {
            Command command;
            command.kind  = CommandKind::StreamViews;
            command.ticks = mode;
            return Parsed(std::move(command));
        }
    }
    return Error(L"stream-views takes off, p2 or both");
}

ParsedLine ParseNpcDetail(const std::vector<std::wstring_view>& words)
{
    constexpr std::wstring_view kModes[] = {L"off", L"reduced", L"full"};
    for (std::uint32_t mode = 0; words.size() == 2 && mode < std::size(kModes); ++mode)
    {
        if (words[1] == kModes[mode] || (mode == 2 && words[1] == L"on"))
        {
            Command command;
            command.kind  = CommandKind::NpcDetail;
            command.ticks = mode;
            return Parsed(std::move(command));
        }
    }
    return Error(L"npc-detail takes full, reduced or off (on means full)");
}

ParsedLine ParseStreamFov(const std::vector<std::wstring_view>& words)
{
    constexpr unsigned long kMaxDegrees = 170;  // views::kMaxStreamFov
    Command command;
    command.kind = CommandKind::StreamFovPlayerOne;
    if (words.size() == 2 && words[1] == L"auto")
    {
        return Parsed(std::move(command));
    }
    const std::wstring text  = words.size() == 2 ? std::wstring(words[1]) : std::wstring();
    wchar_t*           end   = nullptr;
    const auto         value = std::wcstoul(text.c_str(), &end, 10);
    if (text.empty() || text[0] == L'-' || end != text.c_str() + text.size() || value < 1 || value > kMaxDegrees)
    {
        return Error(std::format(L"stream-fov-p1 takes auto or whole degrees from 1 to {}", kMaxDegrees));
    }
    command.ticks = static_cast<std::uint32_t>(value);
    return Parsed(std::move(command));
}

ParsedLine ParseBare(CommandKind kind, const std::vector<std::wstring_view>& words)
{
    if (words.size() != 1)
    {
        return Error(std::format(L"{} takes nothing after it", words[0]));
    }
    Command command;
    command.kind = kind;
    return Parsed(std::move(command));
}

/// Shortest text that reads back as the same float ("1", "-0.5", "0.25").
std::wstring AxisText(float value)
{
    return std::format(L"{}", value);
}

}  // namespace

ParsedLine ParseLine(std::wstring_view line)
{
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == L'#')
    {
        return {};
    }
    const auto words = Words(trimmed);
    const auto name  = words[0];
    if (name == L"join") return ParseBare(CommandKind::Join, words);
    if (name == L"leave") return ParseBare(CommandKind::Leave, words);
    if (name == L"quit") return ParseBare(CommandKind::Quit, words);
    if (name == L"status") return ParseBare(CommandKind::Status, words);
    if (name == L"own-camera") return ParseSwitch(CommandKind::OwnCamera, words, L"on", L"off");
    if (name == L"sticks") return ParseSwitch(CommandKind::Sticks, words, L"per-pad", L"merged");
    if (name == L"layout") return ParseSwitch(CommandKind::Layout, words, L"side-by-side", L"top-bottom");
    if (name == L"bars") return ParseSwitch(CommandKind::Bars, words, L"on", L"off");
    if (name == L"split-fov") return ParseSplitFov(words);
    if (name == L"hud-band") return ParseSwitch(CommandKind::HudBand, words, L"on", L"off");
    if (name == L"npc-centre") return ParseSwitch(CommandKind::NpcCentre, words, L"on", L"off");
    if (name == L"npc-detail") return ParseNpcDetail(words);
    if (name == L"hud-probe") return ParseSwitch(CommandKind::HudProbe, words, L"on", L"off");
    if (name == L"hud-trace") return ParseHudTrace(words);
    if (name == L"hud-image") return ParseSwitch(CommandKind::HudImage, words, L"on", L"off");
    if (name == L"hud-levers") return ParseSwitch(CommandKind::HudLevers, words, L"on", L"off");
    if (name == L"hud-levers-mask") return ParseMask(words, CommandKind::HudLeversMask, 511);
    if (name == L"sticks-mask") return ParseMask(words, CommandKind::SticksMask, 15);
    if (name == L"leak-fixes") return ParseMask(words, CommandKind::LeakFixes, 7);
    if (name == L"buttons-mask") return ParseMask(words, CommandKind::ButtonsMask, 3);
    if (name == L"icons-mask") return ParseMask(words, CommandKind::IconsMask, 15);
    if (name == L"battle-mask") return ParseMask(words, CommandKind::BattleMask, 511);
    if (name == L"perf-log") return ParseSwitch(CommandKind::PerfLog, words, L"on", L"off");
    if (name == L"single-view") return ParseSingleView(words);
    if (name == L"stream-views") return ParseStreamViews(words);
    if (name == L"stream-fov-p1") return ParseStreamFov(words);
    if (name == L"stream-feedback") return ParseSwitch(CommandKind::StreamFeedback, words, L"p1", L"each");
    if (name == L"minimap-once") return ParseSwitch(CommandKind::MinimapOnce, words, L"on", L"off");
    if (name == L"framing-offset") return ParseFramingOffset(words);
    if (name == L"shoulder-scale") return ParseSwitch(CommandKind::ShoulderScale, words, L"on", L"off");
    if (name == L"shoulder-scale-min") return ParseShoulderMinimum(words);
    if (name == L"dump") return ParseDump(words);
    if (name == L"stick") return ParseStick(words);
    if (name == L"wait") return ParseWait(words);
    if (name == L"wait-until") return ParseWaitUntil(words);
    if (name == L"console") return ParseText(CommandKind::Console, name, trimmed);
    if (name == L"note") return ParseText(CommandKind::Note, name, trimmed);
    return Error(std::format(L"unknown command '{}'", name));
}

std::wstring Describe(const Command& command)
{
    switch (command.kind)
    {
        case CommandKind::Join: return L"join";
        case CommandKind::Leave: return L"leave";
        case CommandKind::Quit: return L"quit";
        case CommandKind::Status: return L"status";
        case CommandKind::OwnCamera: return command.on ? L"own-camera on" : L"own-camera off";
        case CommandKind::Sticks: return command.on ? L"sticks per-pad" : L"sticks merged";
        case CommandKind::Layout: return command.on ? L"layout side-by-side" : L"layout top-bottom";
        case CommandKind::Bars: return command.on ? L"bars on" : L"bars off";
        case CommandKind::HudBand: return command.on ? L"hud-band on" : L"hud-band off";
        case CommandKind::NpcCentre: return command.on ? L"npc-centre on" : L"npc-centre off";
        case CommandKind::NpcDetail:
            return command.ticks == 2 ? L"npc-detail full" : command.ticks == 1 ? L"npc-detail reduced" : L"npc-detail off";
        case CommandKind::HudProbe: return command.on ? L"hud-probe on" : L"hud-probe off";
        case CommandKind::HudTrace: return std::format(L"hud-trace {}", command.ticks);
        case CommandKind::HudImage: return command.on ? L"hud-image on" : L"hud-image off";
        case CommandKind::HudLevers: return command.on ? L"hud-levers on" : L"hud-levers off";
        case CommandKind::HudLeversMask: return std::format(L"hud-levers-mask {}", command.ticks);
        case CommandKind::SticksMask: return std::format(L"sticks-mask {}", command.ticks);
        case CommandKind::LeakFixes: return std::format(L"leak-fixes {}", command.ticks);
        case CommandKind::ButtonsMask: return std::format(L"buttons-mask {}", command.ticks);
        case CommandKind::IconsMask: return std::format(L"icons-mask {}", command.ticks);
        case CommandKind::BattleMask: return std::format(L"battle-mask {}", command.ticks);
        case CommandKind::PerfLog: return command.on ? L"perf-log on" : L"perf-log off";
        case CommandKind::SingleView:
            return command.ticks == 1 ? L"single-view on" : command.ticks == 2 ? L"single-view off" : L"single-view auto";
        case CommandKind::StreamViews:
            return command.ticks == 1 ? L"stream-views p2" : command.ticks == 2 ? L"stream-views both" : L"stream-views off";
        case CommandKind::StreamFovPlayerOne:
            return command.ticks == 0 ? std::wstring(L"stream-fov-p1 auto") : std::format(L"stream-fov-p1 {}", command.ticks);
        case CommandKind::StreamFeedback: return command.on ? L"stream-feedback p1" : L"stream-feedback each";
        case CommandKind::MinimapOnce: return command.on ? L"minimap-once on" : L"minimap-once off";
        case CommandKind::ShoulderScale: return command.on ? L"shoulder-scale on" : L"shoulder-scale off";
        case CommandKind::ShoulderScaleMin: return L"shoulder-scale-min " + AxisText(command.blend);
        case CommandKind::FramingOffset:
            return std::format(L"framing-offset {} {}", command.offsets[0], command.offsets[1]);
        case CommandKind::Dump: return L"dump " + command.text;
        case CommandKind::SplitFov:
            return command.blend < 0.0f ? std::wstring(L"split-fov native") : L"split-fov " + AxisText(command.blend);
        case CommandKind::Stick:
            return std::format(L"stick {} {} {} {} {} {}", command.player, AxisText(command.axes[0]),
                               AxisText(command.axes[1]), AxisText(command.axes[2]), AxisText(command.axes[3]),
                               command.ticks);
        case CommandKind::Wait: return std::format(L"wait {}", command.ticks);
        case CommandKind::WaitUntil:
            return std::format(L"wait-until {} {}", command.condition == WaitCondition::Playing ? L"playing" : L"out",
                               command.ticks);
        case CommandKind::Console: return L"console " + command.text;
        case CommandKind::Note: return L"note " + command.text;
    }
    return L"(unknown)";
}

}  // namespace lss::control
