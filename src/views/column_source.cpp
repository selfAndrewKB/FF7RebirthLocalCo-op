#include "views/column_source.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <format>

namespace lss::views::column_source {

const wchar_t* NameOf(Why why) noexcept
{
    switch (why)
    {
        case Why::None: return L"their own";
        case Why::WholeScreen: return L"the game takes the whole screen";
        case Why::SharedRide: return L"player 1 rides a vehicle the whole party shares";
        case Why::Handed: return L"the game has their party member";
        case Why::Console: return L"single-view on";
        case Why::NoPlayer: return L"no player's column";
    }
    return L"?";
}

std::wstring Describe(int seat, const SeatSource& before, const SeatSource& after)
{
    if (before == after)
    {
        return {};
    }
    const wchar_t* name = players::SeatName(seat);
    if (after.source == Source::Self)
    {
        return std::format(L"{}'s view is their own again", name);
    }
    const bool screen = after.source == Source::Screen;  // no column of theirs: the engine's single view
    if (after.why == Why::Handed)
    {
        return screen ? std::format(L"{} waits for a party member (the game has it): player 1's view alone until {} "
                                    L"takes one",
                                    name, name)
                      : std::format(L"{}'s view shows player 1's picture while the game has their party member "
                                    L"(the others play on; their column stays on the row, and on one monitor their "
                                    L"view leaves the screen after a moment)",
                                    name);
    }
    return screen ? std::format(L"{}'s view gives way to player 1's: {}", name, NameOf(after.why))
                  : std::format(L"{}'s view shows player 1's camera and HUD: {}", name, NameOf(after.why));
}

#if LSS_DEV

void RegisterCommands(Override& choice)
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"single-view",
                       .shape = control::Shape::Word,
                       .words = {L"auto", L"on", L"off"},
                       .help  = L"player 1's picture in every further player's view (one monitor: player 1's single "
                                L"view) for the game's whole-screen states (auto, the default), always, or never",
                       .set   = [&choice](const Value& v) {
                           const std::wstring& mode = std::get<std::wstring>(v);
                           // Applied at the next tick, which logs the change.
                           if (mode == L"on")
                           {
                               choice = Override::Single;
                               return Outcome{true, L"player 1's picture while a further player is in"};
                           }
                           if (mode == L"off")
                           {
                               choice = Override::Split;
                               return Outcome{true, L"every player's own view, always"};
                           }
                           choice = Override::Auto;
                           return Outcome{true, L"player 1's picture for the game's whole-screen states (auto)"};
                       },
                       .get = [&choice] {
                           return Value{std::wstring(choice == Override::Single  ? L"on"
                                                     : choice == Override::Split ? L"off"
                                                                                   : L"auto")};
                       }});
}

#endif

}  // namespace lss::views::column_source
