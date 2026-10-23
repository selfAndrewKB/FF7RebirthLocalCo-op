#include "players/split_layout.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <format>

namespace lss::players {

namespace {

std::uint8_t Other(std::uint8_t layout) noexcept
{
    return layout == static_cast<std::uint8_t>(SplitLayoutKind::SideBySide)
               ? static_cast<std::uint8_t>(SplitLayoutKind::TopBottom)
               : static_cast<std::uint8_t>(SplitLayoutKind::SideBySide);
}

}  // namespace

HeldChange SplitLayout::Update(bool playerTwoIn) noexcept
{
    return held_.Update(playerTwoIn, chosen_);  // nothing chosen: the game's layout stays
}

HeldChange SplitLayout::Toggle() noexcept
{
    // Switch from what is on screen: the chosen layout if there is one, otherwise the game's.
    std::uint8_t from = 0;
    if (chosen_)
    {
        from = *chosen_;
    }
    else if (const auto current = held_.Read())
    {
        from = *current;
    }
    else
    {
        return {HeldChange::Kind::Unreadable};  // the other layout is unknown; nothing chosen
    }
    return Choose(Other(from));
}

HeldChange SplitLayout::Choose(std::uint8_t value) noexcept
{
    chosen_ = value;
    return held_.In() ? held_.Hold(value) : HeldChange{HeldChange::Kind::Chosen, 0, value};
}

std::wstring LayoutText(std::int32_t value)
{
    const wchar_t* name = value == static_cast<std::int32_t>(SplitLayoutKind::SideBySide) ? L"side by side"
                          : value == static_cast<std::int32_t>(SplitLayoutKind::TopBottom) ? L"top/bottom"
                                                                                           : L"another value";
    return std::format(L"{} ({})", value, name);
}

#if LSS_DEV

void RegisterCommands(SplitLayout& layout)
{
    using control::Outcome;
    using control::Value;
    control::Register(
        {.name  = L"layout",
         .shape = control::Shape::Word,
         .words = {L"top-bottom", L"side-by-side"},
         .help  = L"how the screen is split (now, or from the next join); reads the layout in force, chosen or the "
                  L"game's",
         .set   = [&layout](const Value& v) {
             using Kind            = HeldChange::Kind;
             const bool sideBySide = std::get<std::wstring>(v) == L"side-by-side";
             const auto change     = layout.Choose(static_cast<std::uint8_t>(sideBySide ? SplitLayoutKind::SideBySide
                                                                                        : SplitLayoutKind::TopBottom));
             const wchar_t* name   = sideBySide ? L"side by side" : L"top/bottom";
             switch (change.kind)
             {
                 case Kind::Applied:
                 case Kind::AlreadySet: return Outcome{true, name};
                 case Kind::Chosen: return Outcome{true, std::wstring(name) + L" from the next join"};
                 case Kind::ApplyFailed: return Outcome{false, L"TwoPlayerSplitscreenLayout could not be read or set"};
                 default: return Outcome{false, L"unexpected layout outcome"};
             }
         },
         .get = [&layout] {
             // Always one of the entry's words, so `wait-until layout == ...` compares; empty only when unreadable.
             const auto current = layout.Current();
             if (!current)
             {
                 return Value{};
             }
             return Value{std::wstring(*current == static_cast<std::uint8_t>(SplitLayoutKind::SideBySide)
                                           ? L"side-by-side"
                                           : L"top-bottom")};
         }});
}

#endif

}  // namespace lss::players
