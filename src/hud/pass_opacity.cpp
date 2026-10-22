#include "hud/pass_opacity.h"

#include "hud/field_markers.h"
#include "hud/prompt_rows.h"
#include "hud/search_pins.h"
#include "hud/widget_opacity.h"
#include "player_two/command_menu_view.h"
#include "player_two/weak_object.h"

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
    const float previous = widget_opacity::Get(widget);
    if (previous == 0.0f)
    {
        return false;  // not drawn anyway
    }
    if (weak == 0 && !player_two::weak_object::Assign(&weak, widget))
    {
        return false;
    }
    if (!widget_opacity::Set(widget, 0.0f))
    {
        return false;
    }
    try
    {
        hidden_.push_back({weak, previous});
    }
    catch (...)
    {
        widget_opacity::Set(widget, previous);  // not remembered: not left transparent
        return false;
    }
    return true;
}

void HiddenSet::RestoreAll() noexcept
{
    for (const Hidden& hidden : hidden_)
    {
        void* widget = player_two::weak_object::ObjectOf(hidden.weak);
        if (widget != nullptr && widget_opacity::Get(widget) == 0.0f)
        {
            widget_opacity::Set(widget, hidden.previous);
        }
    }
    hidden_.clear();
}

void HiddenSet::Forget() noexcept
{
    hidden_.clear();
}

namespace pass_opacity {

void ShowOnly(int player) noexcept
{
    prompt_rows::ShowOnly(player);
    field_markers::ShowOnly(player);
    search_pins::ShowOnly(player);
    player_two::command_menu_view::ShowOnly(player);
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
