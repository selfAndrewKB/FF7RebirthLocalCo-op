#include "hud/passes/pass_opacity.h"

#include "hud/field/field_markers.h"
#include "hud/field/prompt_rows.h"
#include "hud/field/search_pins.h"
#include "player_two/battle/command_menu_view.h"
#include "player_two/weak_object.h"
#include "players/widget_opacity.h"

namespace lss::hud {

void HiddenSet::Reserve(std::size_t count) noexcept
{
    try
    {
        hidden_.reserve(count);
    }
    catch (...)
    {
    }
}

bool HiddenSet::Hide(void* widget, std::uint64_t weak) noexcept
{
    if (widget == nullptr)
    {
        return false;
    }
    const float previous = players::widget_opacity::Get(widget);
    if (previous == 0.0f)
    {
        return false;  // not drawn anyway
    }
    if (weak == 0 && !player_two::weak_object::Assign(&weak, widget))
    {
        return false;
    }
    if (!players::widget_opacity::Set(widget, 0.0f))
    {
        return false;
    }
    try
    {
        hidden_.push_back({weak, previous});
    }
    catch (...)
    {
        players::widget_opacity::Set(widget, previous);  // not remembered: not left transparent
        return false;
    }
    return true;
}

void HiddenSet::RestoreAll() noexcept
{
    for (const Hidden& hidden : hidden_)
    {
        void* widget = player_two::weak_object::ObjectOf(hidden.weak);
        if (widget != nullptr && players::widget_opacity::Get(widget) == 0.0f)
        {
            players::widget_opacity::Set(widget, hidden.previous);
        }
    }
    hidden_.clear();
}

void HiddenSet::Forget() noexcept
{
    hidden_.clear();
}

namespace pass_opacity {

void ShowOnly(int seat) noexcept
{
    prompt_rows::ShowOnly(seat);
    field_markers::ShowOnly(seat);
    search_pins::ShowOnly(seat);
    player_two::command_menu_view::ShowOnly(seat);
}

void RestoreAll() noexcept
{
    prompt_rows::Restore();
    field_markers::Restore();
    search_pins::Restore();
    player_two::command_menu_view::Restore();
}

}  // namespace pass_opacity

}  // namespace lss::hud
