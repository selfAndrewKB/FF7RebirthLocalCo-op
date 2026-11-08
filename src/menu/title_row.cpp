#include "menu/title_row.h"

#include "facts/menu.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page_open.h"
#include "menu/game_ui.h"
#include "menu/title_row_rules.h"
#include "platform/call_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/unreal_access.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace lss::menu::title_row {

namespace {

using game_ui::At;
using platform::GameArray;

using ConstructFn = void (*)(void* start);
using GrowFn      = void (*)(void* array);
using FindFn      = void* (*)(void* widget, const void* name);
using SetCountFn  = void (*)(void* box, std::int32_t count);
using RefreshFn   = void (*)(void* box);
using IsAFn       = bool (*)(void* object);  // the game's class tests (Cast_*: a bool in al)

constexpr int kMostRows = 16;  // far more than the game builds: a longer list is not the one the research read

/// Where the row's pieces are, found at install.
struct Game
{
    std::uintptr_t pressAt = 0, textSlot = 0, textWas = 0, listName = 0;
    GrowFn         grow       = nullptr;
    FindFn         find       = nullptr;
    SetCountFn     setCount   = nullptr;
    RefreshFn      refresh    = nullptr;
    IsAFn          isBox        = nullptr;
};

// Set at install and on the first tick, then only read.
Game                    g;
platform::VTableEntry   g_construct, g_text;
platform::CallRedirect  g_press;  // the press switch's call of the kind's handler
bool                    g_placed = false;

// Game thread.
std::uint64_t g_inserted = 0;
std::uint64_t g_failed   = 0;

/// The kind's place taken: the array grown the way the construct grows it (Num raised first), checked to have grown,
/// the kinds below moved down. Raw: inside a guard. False (Num back) if it did not grow.
bool InsertRaw(GameArray* kinds, std::size_t place)
{
    const std::int32_t was = kinds->num;
    kinds->num             = was + 1;
    if (kinds->num > kinds->max)
    {
        g.grow(kinds);
    }
    if (kinds->num > kinds->max || kinds->data == nullptr)
    {
        kinds->num = was;
        return false;
    }
    auto* const data = static_cast<std::int32_t*>(kinds->data);
    std::memmove(data + place + 1, data + place, (static_cast<std::size_t>(was) - place) * sizeof(std::int32_t));
    data[place] = facts::kStartMenuKindCoop;
    return true;
}

bool InsertGuarded(GameArray* kinds, std::size_t place) noexcept
{
    const std::int32_t was = kinds->num;
    __try
    {
        return InsertRaw(kinds, place);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        kinds->num = was;  // nothing half-built counted
        return false;
    }
}

/// The kind taken out again (the list's count could not follow it): the kinds below moved back up.
void RemoveGuarded(GameArray* kinds, std::size_t place) noexcept
{
    __try
    {
        auto* const data = static_cast<std::int32_t*>(kinds->data);
        std::memmove(data + place, data + place + 1, (static_cast<std::size_t>(kinds->num) - place - 1) * sizeof(std::int32_t));
        --kinds->num;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
    }
}

/// `object` is of `cls` (UE4SS's class walk, under a guard: the object is a raw pointer read from the game).
bool IsAGuarded(RC::Unreal::UObject* object, RC::Unreal::UClass* cls, bool& is) noexcept
{
    __try
    {
        is = object->IsA(cls);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

// The canvas slot's class and its reflected SetSize (stock UMG), found at the first use (game thread).
RC::Unreal::UClass*         g_canvasSlotClass = nullptr;
RC::Unreal::UFunction*      g_setSize         = nullptr;
std::optional<std::int32_t> g_setSizeAt;  // InSize's place in the call's parameters

bool SetSizeReady() noexcept
try
{
    if (g_canvasSlotClass == nullptr)
    {
        g_canvasSlotClass =
            RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, L"/Script/UMG.CanvasPanelSlot");
    }
    if (g_setSize == nullptr || !g_setSizeAt)
    {
        g_setSize = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
            nullptr, nullptr, L"/Script/UMG.CanvasPanelSlot:SetSize");
        g_setSizeAt = g_setSize != nullptr ? players::unreal::ParamOffset(g_setSize, L"InSize", 8) : std::nullopt;
    }
    return g_canvasSlotClass != nullptr && g_setSize != nullptr && g_setSizeAt && *g_setSizeAt + 8 <= 24 &&
           g_setSize->GetParmsSize() <= 32;
}
catch (...)
{
    return false;
}

/// The list told of its new length, as the construct's last steps do. Each step guarded; `counted` says whether the list
/// took the new count (until then the row can be taken out again).
const wchar_t* ShowInList(void* start, std::int32_t rows, bool& counted) noexcept
{
    counted = false;
    void* list  = nullptr;
    bool  isBox = false;
    if (!platform::CallGuarded(g.find, list, start, reinterpret_cast<const void*>(g.listName)) || list == nullptr ||
        !platform::CallGuarded(g.isBox, isBox, list) || !isBox)
    {
        return L"its list could not be found";
    }
    if (!platform::CallGuarded(g.setCount, list, rows))
    {
        platform::LogWarning(L"Co-op menu: the title list's count faulted ({})",
                             platform::DescribeFault(platform::LastFault()).text);
        return L"its list's count could not be set";
    }
    counted = true;
    if (!platform::CallGuarded(g.refresh, list))
    {
        platform::LogWarning(L"Co-op menu: the title list's refresh faulted ({})",
                             platform::DescribeFault(platform::LastFault()).text);
        return L"its list could not be refreshed";
    }
    // The height, as the construct sets it: the canvas slot's stock SetSize, reflected (the slot's class checked first).
    float       width = 0.0f, base = 0.0f;
    auto* const slot  = static_cast<RC::Unreal::UObject*>(platform::ReadPointer(list, facts::kWidgetSlot));
    if (slot == nullptr || !platform::Read(At(start, facts::kStartMenuListWidth), width) ||
        !platform::Read(At(start, facts::kStartMenuListBase), base) || !SetSizeReady())
    {
        return L"its list's height could not be read";
    }
    bool canvas = false;
    if (!IsAGuarded(slot, g_canvasSlotClass, canvas))
    {
        return L"its list's slot could not be read";
    }
    if (!canvas)
    {
        // The construct sizes the list only in a canvas slot: in any other its panel sizes it.
        platform::LogDebug(L"Co-op menu: the title list's slot is no canvas slot; no height to set, as the game sets none");
        return nullptr;
    }
    const float height = static_cast<float>(rows - facts::kStartMenuBaseRows) * facts::kStartMenuRowHeight + base;
    alignas(16) std::uint8_t frame[32]{};
    std::memcpy(frame + *g_setSizeAt, &width, sizeof width);
    std::memcpy(frame + *g_setSizeAt + sizeof width, &height, sizeof height);
    return players::unreal::GuardedProcessEvent(slot, g_setSize, frame) ? nullptr : L"its list's height could not be set";
}

/// The Co-op row below Options in a list the construct just built.
void AddRow(void* start) noexcept
{
    GameArray kinds;
    if (!platform::Read(At(start, facts::kStartMenuKinds), kinds) || kinds.data == nullptr || kinds.num <= 0 ||
        kinds.num >= kMostRows)
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: the title menu's rows could not be read ({}); no Co-op row there", kinds.num);
        return;
    }
    std::array<std::int32_t, kMostRows> read{};
    if (!platform::CopyGuarded(read.data(), kinds.data, static_cast<std::size_t>(kinds.num) * sizeof(std::int32_t)))
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: the title menu's rows could not be copied; no Co-op row there");
        return;
    }
    const auto place = title_row_rules::PlaceBelow(std::span(read.data(), static_cast<std::size_t>(kinds.num)),
                                                   facts::kStartMenuKindOptions, facts::kStartMenuKindCoop);
    if (!place)
    {
        platform::LogDebug(L"Co-op menu: the title menu's {} rows need no Co-op row added", kinds.num);
        return;
    }
    auto* const live = reinterpret_cast<GameArray*>(At(start, facts::kStartMenuKinds));
    if (!InsertGuarded(live, *place))
    {
        ++g_failed;
        platform::LogWarning(L"Co-op menu: inserting the title menu's Co-op row faulted");
        return;
    }
    bool counted = false;
    if (const wchar_t* failed = ShowInList(start, kinds.num + 1, counted))
    {
        ++g_failed;
        if (!counted)
        {
            RemoveGuarded(live, *place);  // the list keeps its count: the rows and their kinds stay in step
        }
        platform::LogWarning(L"Co-op menu: the title menu's Co-op row {}: {}", counted ? L"is in its rows" : L"was taken out again",
                             failed);
        return;
    }
    ++g_inserted;
    platform::LogDebug(L"Co-op menu: a Co-op row in the title menu, place {} of {}", *place, kinds.num + 1);
}

/// The start menu's construct (vtable slot): the game's, then the row.
void Construct(void* start)
{
    reinterpret_cast<ConstructFn>(g_construct.Original())(start);
    AddRow(start);
}

/// The kind's press (its one caller is the row press's switch): the page, when the start menu has no flow of its own
/// running, the test the game's handler for the kind makes.
void Press(void* start)
{
    std::int32_t action = -1;
    if (platform::Read(At(start, facts::kStartMenuAction), action) && action == 0)
    {
        coop_page_open::OpenAtTitle(start);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f         = facts;
    const auto press    = platform::FindFunction(image, f::kStartMenuKindCoopPress);
    const auto grow     = platform::FindFunction(image, f::kIntArrayGrow);
    const auto find     = platform::FindFunction(image, f::kFindWidget);
    const auto setCount = platform::FindFunction(image, f::kScrollBoxSetCount);
    const auto refresh  = platform::FindFunction(image, f::kScrollBoxRefresh);
    const auto castBox  = platform::FindFunction(image, f::kCastScrollBox);
    const auto textSlot = image.Rebase(f::kStartMenuRowTextSlot, 8);
    const auto textWas  = image.Rebase(f::kStartMenuRowTextWas);
    const auto listName = image.Rebase(f::kNameListMenu, 8);
    if (!press || !grow || !find || !setCount || !refresh || !castBox || !textSlot || !textWas ||
        !listName || !coop_page_open::InstallTitle(image))
    {
        platform::LogWarning(L"Co-op menu: a check of the title menu failed; no Co-op row there (the main menu's stays)");
        return false;
    }
    g.pressAt  = *press;
    g.grow     = reinterpret_cast<GrowFn>(*grow);
    g.find     = reinterpret_cast<FindFn>(*find);
    g.setCount = reinterpret_cast<SetCountFn>(*setCount);
    g.refresh  = reinterpret_cast<RefreshFn>(*refresh);
    g.isBox        = reinterpret_cast<IsAFn>(*castBox);
    g.textSlot = *textSlot;
    g.textWas  = *textWas;
    g.listName = *listName;
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    g_placed = g_press.Install(image, facts::kStartMenuKindCoopCall, g.pressAt, reinterpret_cast<std::uintptr_t>(&Press)) &&
               g_text.Replace(g.textSlot, g.textWas, reinterpret_cast<std::uintptr_t>(coop_menu_rules::kRowLabel)) &&
               platform::ReplaceSlot(image, facts::kStartMenuConstruct, reinterpret_cast<std::uintptr_t>(&Construct),
                                     g_construct);
    if (g_placed)
    {
        platform::Log(L"Co-op menu: a Co-op row in the title menu below Options (the demo's Buy Now row, which the full "
                      L"game never shows)");
    }
    else
    {
        Unplace();
        platform::LogWarning(L"Co-op menu: the title menu's row could not be placed; no Co-op row there (the main menu's "
                             L"stays)");
    }
    return g_placed;
}

void Unplace() noexcept
{
    g_construct.Restore();
    g_text.Restore();
    g_press.Restore();
    g_placed = false;
}

std::wstring Reading()
{
    return std::format(L"; title row {}, added {}, failed {}", g_placed ? L"placed" : L"not placed", g_inserted, g_failed);
}

}  // namespace lss::menu::title_row
