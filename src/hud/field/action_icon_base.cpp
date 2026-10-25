#include "hud/field/action_icon_base.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include "facts/hud.h"
#include "hud/field/action_rows.h"
#include "hud/field/prompt_rows.h"
#include "hud/layout/viewport_placement.h"
#include "hud/user_widgets.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "players/tracked_object.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::action_icon_base {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UObject;

// RE-p2-hud-view-levers.md §1 and RE-p2-field-prompts.md §4 (this build).
constexpr std::int32_t   kBasePos     = 0x3C8;        // UEndFieldActionIcon._BasePosY, an FVector2D

constexpr int kMaxOwnBaseLines = 20;

using Tracked = players::TrackedObject;


/// A widget class: whether it is a field-action icon class, and if so its default's base.
struct IconClass
{
    bool        icon = false;
    Tracked     defaultObject;
    LayoutPoint original;  // the class default's base, as the game has it
    LayoutPoint anchor;    // the corner of the 1920x1080 frame it belongs to
};

// Set once at install.
bool g_ready = false;  // the list and the game's layout were found (hud/field/action_rows.h)

// Game thread.
std::unordered_map<const void*, IconClass> g_classes;
LayoutSize                                 g_applied = kSoloLayout;  // the layout the bases are for
std::vector<const UObject*>                g_ownBaseLogged;
int                                        g_ownBaseLines = 0;

using platform::Read;  // the guarded reads: platform/guarded.h

bool ReadBase(const UObject* object, LayoutPoint& base) noexcept
{
    return Read(reinterpret_cast<std::uintptr_t>(object) + kBasePos, base);
}

bool WriteBase(UObject* object, const LayoutPoint& base) noexcept
{
    return platform::WriteGuarded(reinterpret_cast<std::uint8_t*>(object) + kBasePos, &base, sizeof(base));
}

LayoutPoint BaseFor(const IconClass& type, const LayoutSize& layout) noexcept
{
    return KeepDistanceFromAnchor(type.original, type.anchor, layout.width, layout.height);
}

/// Moves `object`'s base from where `type`'s rule puts it for `from` to where it puts it for `to`.
/// A base that is not the rule's for `from` is the object's own: left alone, false.
bool Shift(UObject* object, const IconClass& type, const LayoutSize& from, const LayoutSize& to) noexcept
{
    LayoutPoint now;
    return ReadBase(object, now) && now == BaseFor(type, from) && WriteBase(object, BaseFor(type, to));
}

void LogOwnBase(const UObject* object, const LayoutPoint& now, const LayoutPoint& expected) noexcept
{
    if (g_ownBaseLines >= kMaxOwnBaseLines ||
        std::find(g_ownBaseLogged.begin(), g_ownBaseLogged.end(), object) != g_ownBaseLogged.end())
    {
        return;
    }
    ++g_ownBaseLines;
    g_ownBaseLogged.push_back(object);
    platform::Log(L"field-action icons: an icon's base is ({:.0f},{:.0f}), not its class's ({:.0f},{:.0f}) for this "
                  L"layout; set by something else (a Blueprint?), left alone",
                  now.x, now.y, expected.x, expected.y);
}

struct ShiftCount
{
    int icons    = 0;
    int defaults = 0;
    int own      = 0;  // live icons with a base of their own, left alone
};

/// Moves the bases of every live icon of the classes `only` selects, and of their class defaults.
template <class Select>
ShiftCount ShiftAll(const LayoutSize& from, const LayoutSize& to, Select&& only) noexcept
{
    ShiftCount count;
    for (const auto& live : user_widgets::Live())
    {
        const auto found = g_classes.find(live.cls);
        if (found == g_classes.end() || !found->second.icon || !only(found->first) || !user_widgets::Alive(live))
        {
            continue;
        }
        if (Shift(live.object, found->second, from, to))
        {
            ++count.icons;
        }
        else
        {
            ++count.own;
        }
    }
    for (auto& [cls, type] : g_classes)
    {
        if (type.icon && only(cls) && type.defaultObject.Alive() && Shift(type.defaultObject.object, type, from, to))
        {
            ++count.defaults;
        }
    }
    return count;
}

/// The widget's class, looked up once: an icon class has `_BasePosY` where the research read it.
IconClass* ClassOf(UObject* widget)
{
    const void* cls = widget->GetClassPrivate();
    if (const auto found = g_classes.find(cls); found != g_classes.end())
    {
        return &found->second;
    }
    IconClass type;
    auto* property = widget->GetPropertyByNameInChain(STR("_BasePosY"));
    UObject* defaultObject = widget->GetClassPrivate()->GetClassDefaultObject();
    if (property != nullptr && property->GetSize() == sizeof(LayoutPoint) && property->GetOffset_Internal() == kBasePos &&
        defaultObject != nullptr && ReadBase(defaultObject, type.original))
    {
        type.icon          = true;
        type.defaultObject = Tracked::Of(defaultObject);
        type.anchor        = AnchorOf(type.original);
        if (platform::Enabled(platform::Level::Debug))
        {
            platform::LogDebug(
                L"field-action icons: class {} has its base at ({:.0f},{:.0f}), the frame's corner ({:.1f},{:.1f})",
                widget->GetClassPrivate()->GetName(), type.original.x, type.original.y, type.anchor.x, type.anchor.y);
        }
    }
    return &g_classes.emplace(cls, type).first->second;
}

/// The game's list's rows, resolved (hud/field/action_rows.h).
std::vector<UObject*> RowsOf()
{
    static std::vector<action_rows::RowEntry> s_entries;  // scratch, reused
    std::vector<UObject*>                     rows;
    action_rows::Rows(s_entries);
    for (const action_rows::RowEntry& entry : s_entries)
    {
        rows.push_back(static_cast<UObject*>(entry.widget));
    }
    return rows;
}

/// The rows laid out again by the game's layout for `layout`: each player's through prompt_rows
/// while a further player is in, the whole list otherwise. The rows slide to their new slots as the game
/// slides them. A row whose current place is outside the new layout is not painted there, so it is
/// not ticked and would never slide: it is named in a layout call of its own, which places it at
/// once, as the game places a row it adds (the sort and the slots come out the same each call).
void LayOutAgain(const LayoutSize& layout)
{
    if (player_two::generation::AnyActive())
    {
        prompt_rows::LayOutAgain(layout.width, layout.height);
        return;
    }
    const std::vector<UObject*> rows = RowsOf();
    const std::vector<void*>    widgets(rows.begin(), rows.end());
    const bool ok = !action_rows::LayoutAvailable() ||
                    action_rows::LayOut(reinterpret_cast<void*>(action_rows::List()), widgets, true, layout.width,
                                        layout.height, nullptr);
    if (!ok)
    {
        platform::Log(L"field-action icons: the game's layout raised an exception; the rows keep their places until the "
                      L"game lays them out");
    }
}

/// During a split, each row in the list: a class seen for the first time gets its bases moved for
/// the current layout; a row whose base is not the rule's is logged.
void CheckRows()
{
    bool newClass = false;
    for (UObject* row : RowsOf())
    {
        const bool      known = g_classes.contains(row->GetClassPrivate());
        const IconClass* type = ClassOf(row);
        if (!type->icon)
        {
            continue;
        }
        if (!known)  // runs during a split only
        {
            const void* cls   = row->GetClassPrivate();
            const auto  count = ShiftAll(kSoloLayout, g_applied, [cls](const void* c) { return c == cls; });
            platform::Log(L"field-action icons: a new icon class during the split: {} icon(s) and {} class default(s) "
                          L"moved to this layout's base",
                          count.icons, count.defaults);
            newClass = true;
        }
        LayoutPoint now;
        const LayoutPoint expected = BaseFor(*type, g_applied);
        if (ReadBase(row, now) && !(now == expected))
        {
            LogOwnBase(row, now, expected);
        }
    }
    if (newClass)
    {
        LayOutAgain(g_applied);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const bool ready = action_rows::Install(image) && action_rows::LayoutAvailable();
    g_ready          = ready;
    platform::Log(L"field-action icons: {}", ready ? L"their base follows the split layout (the game's layout places them)"
                                                   : L"the list or the game's layout is not what the research read; the "
                                                     L"icons keep the game's base");
    return ready;
}

void Update(LayoutSize layout) noexcept
{
    if (!g_ready)
    {
        return;
    }
    try
    {
        if (!(layout.width > 0.0f) || !(layout.height > 0.0f))
        {
            layout = kSoloLayout;
        }
        // The classes of the rows the game shows now, before any move, so a first split moves them.
        if (!(layout == g_applied))
        {
            for (const auto& live : user_widgets::Live())
            {
                if (!g_classes.contains(live.cls) && user_widgets::Alive(live))
                {
                    ClassOf(live.object);
                }
            }
            const auto count = ShiftAll(g_applied, layout, [](const void*) { return true; });
            platform::Log(L"field-action icons: layout {:.0f}x{:.0f} -> {:.0f}x{:.0f}: {} icon(s) and {} class default(s) "
                          L"moved to keep their corner{}; laid out again by the game",
                          g_applied.width, g_applied.height, layout.width, layout.height, count.icons, count.defaults,
                          count.own > 0 ? L" (some with a base of their own left alone)" : L"");
            g_applied = layout;
            g_ownBaseLogged.clear();
            LayOutAgain(layout);
        }
        if (!(g_applied == kSoloLayout))
        {
            CheckRows();  // the rows of a split; nothing to check at the game's own bases
        }
    }
    catch (...)
    {
        platform::Log(L"field-action icons: failed (an exception); the bases stay as they are");
    }
}

}  // namespace lss::hud::action_icon_base
