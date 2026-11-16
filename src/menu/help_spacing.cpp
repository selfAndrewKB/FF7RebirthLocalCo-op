#include "menu/help_spacing.h"

#include "menu/game_ui.h"
#include "platform/log.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace lss::menu::help_spacing {

namespace {

using RC::Unreal::UObject;
using players::unreal::ObjectProperty;

/// A canvas slot the lift moves, and its own place before the lift.
struct Placed
{
    std::uint64_t slot = 0;  // the slot's weak value
    float         x    = 0.0f;
    float         y    = 0.0f;
};

enum Part : std::size_t
{
    kPanel,       // Pnl_HelpTxt: raised
    kLine,        // Line_Left: lowered
    kLineShadow,  // Line_Left_s: lowered
    kParts,
};

// Game thread.
std::array<Placed, kParts> g_parts{};
std::uint64_t              g_text       = 0;      // Txt_Help's weak value
bool                       g_open       = false;  // the page as last seen
bool                       g_found      = false;  // the footer's parts were found at this opening
float                      g_lineHeight = 0.0f;   // one line's desired height: the least seen on the page
float                      g_lift       = 0.0f;   // as applied

/// The canvas slot of `widget`, with its place now; false if it has none or it cannot be read.
bool Take(UObject* widget, Placed& out)
{
    UObject* slot = widget != nullptr ? ObjectProperty(widget, STR("Slot")) : nullptr;
    if (slot == nullptr || !game_ui::IsCanvasSlot(slot) || !game_ui::SlotPosition(slot, out.x, out.y))
    {
        return false;
    }
    out.slot = players::weak_object::WeakOf(slot);
    return out.slot != 0;
}

/// The footer's help text, its panel and its underline, found once per opening. Null when found, else what is missing.
const wchar_t* Find()
{
    UObject* foot = RC::Unreal::UObjectGlobals::FindFirstOf(STR("Foot_C"));
    if (foot == nullptr)
    {
        return L"no footer widget (Foot_C)";
    }
    UObject* text = ObjectProperty(foot, STR("Txt_Help"));
    UObject* slot = text != nullptr ? ObjectProperty(text, STR("Slot")) : nullptr;
    UObject* panel = slot != nullptr ? ObjectProperty(slot, STR("Parent")) : nullptr;
    if (text == nullptr || panel == nullptr)
    {
        return L"no help text in a panel (Txt_Help)";
    }
    std::array<Placed, kParts> parts{};
    if (!Take(panel, parts[kPanel]) || !Take(ObjectProperty(foot, STR("Line_Left")), parts[kLine]) ||
        !Take(ObjectProperty(foot, STR("Line_Left_s")), parts[kLineShadow]))
    {
        return L"the panel or the underline is not in a canvas slot whose place can be read";
    }
    g_parts = parts;
    g_text  = players::weak_object::WeakOf(text);
    return g_text != 0 ? nullptr : L"the help text has no weak value";
}

/// Every part at its own place moved by `lift` (the panel up, the underline down). False if a part is gone.
bool Apply(float lift) noexcept
{
    bool all = true;
    for (std::size_t i = 0; i < kParts; ++i)
    {
        const Placed& p    = g_parts[i];
        void*         slot = p.slot != 0 ? players::weak_object::ObjectOf(p.slot) : nullptr;
        const float   y    = i == kPanel ? p.y - lift : p.y + lift;
        all = slot != nullptr && game_ui::SetSlotVector(slot, game_ui::SlotVector::Position, p.x, y) && all;
    }
    g_lift = lift;
    return all;
}

void PutBack() noexcept
{
    if (g_found && g_lift != 0.0f && !Apply(0.0f))
    {
        platform::LogDebug(L"help lines: a part of the help bar was gone at the put-back (the game rebuilt it)");
    }
    g_found      = false;
    g_text       = 0;
    g_lineHeight = 0.0f;
    g_lift       = 0.0f;
}

/// The help text's desired height now, or 0 when it cannot be read.
float TextHeight() noexcept
{
    static const auto desired = players::unreal::FindGetter(L"/Script/UMG.Widget:GetDesiredSize", 8);
    static bool       missingSaid = false;
    if (!desired && !std::exchange(missingSaid, true))
    {
        platform::LogWarning(L"help lines: the widget's GetDesiredSize is not found: the help bar stays as the game lays it out");
    }
    void* text = g_text != 0 ? players::weak_object::ObjectOf(g_text) : nullptr;
    float             size[2] = {};
    if (!desired || text == nullptr || !players::unreal::CallGetter(static_cast<UObject*>(text), *desired, size, sizeof size))
    {
        return 0.0f;
    }
    return size[1];
}

}  // namespace

void Tick(bool pageOpen) noexcept
try
{
    if (!pageOpen)
    {
        if (g_open)
        {
            PutBack();
        }
        g_open = false;
        return;
    }
    if (!g_open)
    {
        g_open = true;
        const wchar_t* missing = Find();
        g_found = missing == nullptr;
        if (g_found)
        {
            platform::Log(L"help lines: the help bar's panel at ({:.0f}, {:.0f}) and its underline at ({:.0f}, {:.0f}): a "
                          L"help of several lines lifts the panel and lowers the underline by its extra lines while the page "
                          L"is open",
                          g_parts[kPanel].x, g_parts[kPanel].y, g_parts[kLine].x, g_parts[kLine].y);
        }
        else
        {
            platform::Log(L"help lines: not placed at this opening ({}): a help of several lines hangs below the underline",
                          missing);
        }
    }
    if (!g_found)
    {
        return;
    }
    const float height = TextHeight();
    if (height < 1.0f)
    {
        return;  // no text yet, or unreadable
    }
    g_lineHeight     = g_lineHeight == 0.0f ? height : std::fmin(g_lineHeight, height);
    const float lift = std::round(height - g_lineHeight);
    if (std::fabs(lift - g_lift) < 1.0f)
    {
        return;
    }
    const bool applied = Apply(lift);
    platform::LogDebug(L"help lines: the help is {:.0f} high (one line {:.0f}): the panel {} by {:.0f}{}", height,
                       g_lineHeight, lift > 0.0f ? L"raised" : L"back", lift,
                       applied ? L"" : L" (a part could not be moved)");
}
catch (...)
{
    PutBack();  // the footer's own places again; nothing more this opening
}

}  // namespace lss::menu::help_spacing
