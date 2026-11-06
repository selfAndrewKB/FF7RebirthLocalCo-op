#include "battle/command_menu_view.h"

#include "battle/command_menu.h"
#include "battle/command_menu_shared.h"
#include "facts/command_controller.h"
#include "facts/hud.h"
#include "platform/log.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::command_menu_view {

namespace {

using facts::kCmdFieldMenu;
using facts::kCmdHudMode;
using facts::kCmdMenuWidget;
using facts::kUICommandController;
using facts::kUIKeyGuide;

// A controller's widgets shown per view (RE-p2-command-menu-views.md §10.3, §10.5): the command
// menu, the field menu (the party list) and the wait-mode filter (the tactical-mode tint), each an
// FWeakObjectPtr in the controller. In a seat's pass, the seat's command and field menus take player
// 1's own opacity (their fades), the filter its full opacity (its own content decides).
// The key guide's widget (FWeakObjectPtr at +4 of a KeyGuide53; the game's guide at EndUISystem[53], each seat's own
// built with its controller) shows at full opacity: its own tick (battle/widget_owner) sets its visibility and
// plays its show and hide animations.
constexpr std::uint32_t kWaitFilterWidget = 0x62C;  // FWeakObjectPtr: the controller's wait-mode filter
constexpr std::uint32_t kKeyGuideWidget   = 0x04;   // FWeakObjectPtr: a KeyGuide53's widget
struct Part
{
    std::uint32_t offset;
    bool          borrowOpacity;
    bool          guide;  // the owner is the key guide, not the controller
};
constexpr std::array<Part, 4> kParts{{{kCmdMenuWidget, true, false},
                                      {kCmdFieldMenu, true, false},
                                      {kWaitFilterWidget, false, false},
                                      {kKeyGuideWidget, false, true}}};
// Render (game thread): player 1's widgets (FWeakObjectPtr, resolved again at each use: the game can
// rebuild them) and their own opacity while a seat's pass shows the seat's.
struct Saved
{
    std::uint64_t weak    = 0;
    float         opacity = 1.0F;
};
std::array<Saved, kParts.size()> g_oneSaved{};  // one per part, the key guide's widget last
bool                             g_swapped = false;
int                              g_hides   = 0;  // times the menu widget was found shown and hidden again

void Line(const std::wstring& text) noexcept
{
    platform::Log(L"command menu two: {}", text);
}

/// The owner of a part's widget for a player (`seat` 0 player 1): the controller, or the key guide (the game's, or the
/// seat's own).
std::uintptr_t OwnerOf(const Part& part, int seat, std::uintptr_t controller) noexcept
{
    if (!part.guide)
    {
        return controller;
    }
    return seat != 0 ? reinterpret_cast<std::uintptr_t>(command_menu::KeyGuide(seat))
                     : ReadAt<std::uintptr_t>(command_menu::UiSystem() + kUIKeyGuide);
}

std::uintptr_t ControllerOf(int seat) noexcept
{
    return reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat));
}

/// A controller's widget (its FWeakObjectPtr at `offset`), or null.
void* WidgetOf(std::uintptr_t controller, std::uint32_t offset) noexcept
{
    return controller != 0 ? players::weak_object::ObjectOf(ReadAt<std::uint64_t>(controller + offset)) : nullptr;
}

void SetOpacity(void* widget, float opacity) noexcept
{
    if (widget != nullptr && players::widget_opacity::Get(widget) != opacity)
    {
        players::widget_opacity::Set(widget, opacity);
    }
}

/// Every seat's widgets but `except`'s at render opacity 0.
void HideSeats(int except) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto controller = ControllerOf(seat);
        for (const Part& part : kParts)
        {
            if (seat != except && controller != 0)
            {
                SetOpacity(WidgetOf(OwnerOf(part, seat, controller), part.offset), 0.0F);
            }
        }
    }
}

}  // namespace

void ShowOnly(int seat) noexcept
{
    const auto mine = players::SeatIndex(seat) ? ControllerOf(seat) : 0;
    if (command_menu::UiSystem() == 0 || !players::widget_opacity::Installed() ||
        !players::generation::OnGameThread())
    {
        return;
    }
    const auto one = ReadAt<std::uintptr_t>(command_menu::UiSystem() + kUICommandController);
    if (mine == 0 || ReadAt<std::uint8_t>(mine + kCmdHudMode) == 0 || one == 0)
    {
        return Restore();
    }
    HideSeats(seat);  // a seat whose pass ran before this one
    for (std::size_t i = 0; i < kParts.size(); ++i)
    {
        const Part& part       = kParts[i];
        const auto  oneOwner   = OwnerOf(part, 0, one);
        const auto  seatOwner  = OwnerOf(part, seat, mine);
        void*       oneWidget  = WidgetOf(oneOwner, part.offset);
        void*       seatWidget = WidgetOf(seatOwner, part.offset);
        if (oneWidget == nullptr || seatWidget == nullptr)
        {
            continue;
        }
        if (!g_swapped)
        {
            g_oneSaved[i] = {ReadAt<std::uint64_t>(oneOwner + part.offset), players::widget_opacity::Get(oneWidget)};
        }
        SetOpacity(seatWidget, part.borrowOpacity ? g_oneSaved[i].opacity : 1.0F);
        SetOpacity(oneWidget, 0.0F);
    }
    g_swapped = true;
}

void Restore() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    if (!players::widget_opacity::Installed())
    {
        return;
    }
    if (g_swapped)
    {
        for (auto& saved : g_oneSaved)
        {
            SetOpacity(players::weak_object::ObjectOf(saved.weak), saved.opacity);  // null (skipped) if the game rebuilt it
            saved = {};
        }
        g_swapped = false;
    }
    HideSeats(0);
}

void KeepHidden() noexcept
{
    if (!players::weak_object::Known() || !players::widget_opacity::Installed() || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto controller = ControllerOf(seat);
        for (const Part& part : kParts)
        {
            void* widget = controller != 0 ? WidgetOf(OwnerOf(part, seat, controller), part.offset) : nullptr;
            if (widget == nullptr || players::widget_opacity::Get(widget) == 0.0F)
            {
                continue;
            }
            const bool hidden = players::widget_opacity::Set(widget, 0.0F);
            if (++g_hides <= 5 || g_hides % 100 == 0)
            {
                Line(std::format(L"{}'s widget +{:#x} {} (render opacity 0; {} time(s))", players::SeatName(seat),
                                 part.offset, hidden ? L"hidden" : L"could NOT be hidden", g_hides));
            }
        }
    }
}

}  // namespace lss::battle::command_menu_view
