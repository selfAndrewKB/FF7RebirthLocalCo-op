#include "hud/prompt_rows.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "hud/viewport_placement.h"
#include "hud/widget_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settled_log.h"
#include "player_two/chocobo_ride.h"
#include "player_two/engine_facts.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::prompt_rows {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;

// RE-p2-field-prompts.md §1.2 and §4 (this build).
constexpr std::uintptr_t kEndUISystem       = 0x1493496E0;  // g_EndUISystem (data)
constexpr std::uint32_t  kRowList           = 0x208;        // -> {FWeakObjectPtr* data, int32 num}
constexpr std::uint32_t  kListNum           = 0x8;
constexpr std::uint32_t  kLeaderActions     = 0x598;        // PC -> the leader-action component
constexpr std::uint32_t  kComponentRowBytes = 0x9A8;        // + 12*i: request byte
constexpr std::uint32_t  kComponentRowWeak  = 0x9AC;        // + 12*i: the row's weak widget
constexpr std::uint32_t  kRowStride         = 12;
constexpr std::size_t    kComponentRows     = 14;
constexpr std::uint32_t  kWidgetPriority    = 0x400;
constexpr std::uint32_t  kWidgetRemoveState = 0x418;
constexpr std::uint32_t  kWidgetViewportPosition = 0x220;  // UUserWidget viewport offsets (hud/hud_addresses.h)
constexpr std::int32_t   kMaxRows           = 64;
constexpr int            kMaxLines          = 200;

// FieldActionIconList__SortAndLayout(TArray<FWeakObjectPtr>* list, UObject* placeNow): sorts the
// given array by priority and gives each live row the next slot in it (§4). Called by the game only
// when a row is added or removed.
constexpr std::uintptr_t kSortAndLayout        = 0x1414A9B14;
constexpr std::uint8_t   kSortAndLayoutBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56,
                                                  0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x20, 0x00};
using SortAndLayoutFn = void (*)(void* list, void* placeNow);

// ESlateVisibility.
constexpr std::uint8_t kCollapsed = 1;
constexpr std::uint8_t kHidden    = 2;

struct ListHeader  // a TArray as the layout reads it
{
    std::uint64_t* data = nullptr;
    std::int32_t   num  = 0;
    std::int32_t   max  = 0;
};

struct Row
{
    const UObject* widget    = nullptr;
    std::uint64_t  weak      = 0;  // the list's FWeakObjectPtr, as stored
    int            p2Row     = -1; // player 2's component row naming it now, or -1
    int            p2Ride    = -1; // the handle of player 2's driven chocobo naming it now, or -1
    int            p2Pickup  = -1; // the field-prompt slot of player 2's pass naming it now, or -1
    bool           playerTwo = false;
    std::int32_t   priority  = 0;
    std::int32_t   remove    = 0;
    bool operator==(const Row&) const = default;
};

struct Hidden
{
    std::uint64_t weak     = 0;
    float         previous = 1.0f;  // the row's own render opacity
};

// Set once at install.
std::uintptr_t  g_endUISystem   = 0;
SortAndLayoutFn g_sortAndLayout = nullptr;
UFunction*      g_setOpacity    = nullptr;  // UWidget::SetRenderOpacity
std::int32_t    g_visibilityOffset = -1;
std::int32_t    g_opacityOffset    = -1;

// Game thread.
std::vector<Row>                          g_rows;
struct RowsSteady
{
    std::array<Row, kMaxRows>                rows{};  // the first `count`, each remove state 0
    std::size_t                              count = 0;
    std::array<std::uint8_t, kComponentRows> bytes{};
    bool operator==(const RowsSteady&) const = default;
};
using RowRemoves = std::array<std::int32_t, kMaxRows>;
constexpr std::uint32_t kRowStillFrames = 30;
platform::SettledLog<RowsSteady, RowRemoves> g_rowLog(kRowStillFrames);
std::vector<const UObject*>               g_playerTwoWidgets;  // player 2's until they leave the list
std::vector<std::pair<const UObject*, bool>> g_laidOut;        // the list as last laid out per player
std::vector<Hidden>                       g_hidden;
bool                                      g_layOutAgain = false;  // LayOutAgain: rows outside the layout placed at once
float                                     g_againWidth  = 0.0f;
float                                     g_againHeight = 0.0f;
std::vector<const UObject*>               g_stillTwo;  // scratch, reused
std::vector<std::pair<const UObject*, bool>> g_layoutNow;  // scratch, reused
int                                       g_lines = 0;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

UObject* Resolve(std::uint64_t weakValue)
{
    RC::Unreal::FWeakObjectPtr weak;
    std::memcpy(&weak, &weakValue, sizeof(weakValue));
    return weak.Get();
}

bool LayoutGuarded(ListHeader* list, void* placeNow) noexcept
{
    __try
    {
        g_sortAndLayout(list, placeNow);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The row's viewport position (UUserWidget's viewport offsets Left, Top: what the game's slide sets).
LayoutPoint PositionOf(const UObject* widget) noexcept
{
    LayoutPoint position;
    Read(reinterpret_cast<std::uintptr_t>(widget) + kWidgetViewportPosition, position);
    return position;
}

bool LaidOutBefore(const UObject* widget) noexcept
{
    return std::any_of(g_laidOut.begin(), g_laidOut.end(), [widget](const auto& entry) { return entry.first == widget; });
}

/// Each player's rows laid out on their own, when the list or an owner changed. A row new since the
/// last layout goes straight to its slot, as the game places a row it adds; the others slide.
void LayOutPerPlayer()
{
    g_layoutNow.clear();
    for (const Row& row : g_rows)
    {
        g_layoutNow.emplace_back(row.widget, row.playerTwo);
    }
    if (g_layoutNow == g_laidOut || g_sortAndLayout == nullptr)
    {
        return;
    }
    const bool again = g_layOutAgain;
    g_layOutAgain    = false;
    for (const bool playerTwo : {false, true})
    {
        std::vector<std::uint64_t>  weaks;
        std::vector<const UObject*> widgets;
        const UObject*              placeNow = nullptr;
        for (const Row& row : g_rows)
        {
            if (row.playerTwo == playerTwo)
            {
                weaks.push_back(row.weak);
                widgets.push_back(row.widget);
                placeNow = placeNow == nullptr && row.remove == 0 && !LaidOutBefore(row.widget) ? row.widget : placeNow;
            }
        }
        ListHeader list{weaks.data(), static_cast<std::int32_t>(weaks.size()), static_cast<std::int32_t>(weaks.size())};
        // Each call places the row it names at once and gives the others their slide target (the
        // sort and the slots come out the same each time). After a base change, each row stranded
        // outside the layout is named in a call of its own; the others slide.
        bool laidOut = true;
        bool named   = false;
        for (const UObject* widget : again ? widgets : std::vector<const UObject*>{})
        {
            if (OutsideLayout(PositionOf(widget), g_againWidth, g_againHeight))
            {
                named   = true;
                laidOut = laidOut && LayoutGuarded(&list, const_cast<UObject*>(widget));
            }
        }
        if (!named && !weaks.empty())
        {
            laidOut = LayoutGuarded(&list, const_cast<UObject*>(placeNow));
        }
        if (!laidOut)
        {
            g_sortAndLayout = nullptr;
            platform::Log(L"prompt rows: the game's layout raised an exception; rows keep the shared layout");
            return;
        }
    }
    g_laidOut = g_layoutNow;
}

std::uint8_t VisibilityOf(const UObject* widget) noexcept
{
    std::uint8_t visibility = 0;
    Read(reinterpret_cast<std::uintptr_t>(widget) + static_cast<std::uint32_t>(g_visibilityOffset), visibility);
    return visibility;
}

float OpacityOf(const UObject* widget) noexcept
{
    float opacity = 1.0f;
    Read(reinterpret_cast<std::uintptr_t>(widget) + static_cast<std::uint32_t>(g_opacityOffset), opacity);
    return opacity;
}

/// The game's native setter (hud/widget_opacity.h), or the same function through reflection when its
/// entry could not be checked.
bool SetOpacity(UObject* widget, float opacity)
{
    if (widget_opacity::Installed())
    {
        return widget_opacity::Set(widget, opacity);
    }
    struct
    {
        float opacity;
    } params{opacity};
    return g_setOpacity != nullptr && players::unreal::GuardedProcessEvent(widget, g_setOpacity, &params);
}

/// UWidget's Visibility (one byte) and RenderOpacity (a float) offsets, read by reflection once.
bool FindOffsets(UObject* widget)
{
    if (g_visibilityOffset >= 0)
    {
        return true;
    }
    auto* visibility = widget->GetPropertyByNameInChain(STR("Visibility"));
    auto* opacity    = widget->GetPropertyByNameInChain(STR("RenderOpacity"));
    if (visibility == nullptr || visibility->GetSize() != 1 || opacity == nullptr || opacity->GetSize() != sizeof(float))
    {
        return false;
    }
    g_visibilityOffset = visibility->GetOffset_Internal();
    g_opacityOffset    = opacity->GetOffset_Internal();
    return true;
}

std::wstring Describe(const std::vector<Row>& rows)
{
    std::wstring text;
    for (const Row& row : rows)
    {
        const std::wstring owner = row.p2Row >= 0    ? std::format(L"player 2 row {}", row.p2Row)
                                   : row.p2Ride >= 0 ? std::format(L"player 2's chocobo row {}", row.p2Ride)
                                   : row.p2Pickup >= 0 ? std::format(L"player 2's field prompt slot {}", row.p2Pickup)
                                   : row.playerTwo   ? std::wstring(L"player 2 (leaving)")
                                                   : std::wstring(L"player 1");
        text += std::format(L"{}[{} prio {} remove {}]", text.empty() ? L"" : L" ", owner, row.priority, row.remove);
    }
    return text.empty() ? L"none" : text;
}

void LogIfChanged(const std::array<std::uint8_t, kComponentRows>& bytes)
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_rowLog.Reset();  // off: the next line starts fresh
        return;
    }
    if (g_lines >= kMaxLines)
    {
        return;
    }
    // Which rows, whose, and player 2's requests log at once; a row's remove state steps through its fade
    // over a few frames, so it logs once it holds.
    RowsSteady steady;
    RowRemoves removes{};
    steady.bytes = bytes;
    for (const Row& row : g_rows)
    {
        if (steady.count == steady.rows.size())
        {
            break;  // the read stops at kMaxRows
        }
        removes[steady.count] = row.remove;
        Row& kept             = steady.rows[steady.count++];
        kept                  = row;
        kept.remove           = 0;
    }
    if (!g_rowLog.Offer(steady, removes))
    {
        return;
    }
    std::wstring requests;
    for (std::size_t i = 0; i < kComponentRows; ++i)
    {
        requests += bytes[i] != 0 ? std::format(L" {}:{:#04x}", i, bytes[i]) : L"";
    }
    ++g_lines;
    platform::LogDebug(L"prompt rows: {}; player 2's requests:{}", Describe(g_rows), requests.empty() ? L" none" : requests);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(kEndUISystem, kRowList + sizeof(void*));
    const auto layout = image.Rebase(kSortAndLayout);
    g_endUISystem     = system ? *system : 0;
    if (layout && image.BytesMatch(kSortAndLayout, kSortAndLayoutBytes))
    {
        g_sortAndLayout = reinterpret_cast<SortAndLayoutFn>(*layout);
    }
    try
    {
        g_setOpacity = RC::Unreal::UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                                                 STR("/Script/UMG.Widget:SetRenderOpacity"));
    }
    catch (...)
    {
        g_setOpacity = nullptr;
    }
    const bool native = widget_opacity::Install(image);
    platform::Log(L"prompt rows: per-player layout {}, per-half visibility {}", g_sortAndLayout ? L"ready" : L"UNAVAILABLE",
                  native ? L"ready (native setter)" : g_setOpacity ? L"ready (through reflection)" : L"UNAVAILABLE");
    return g_endUISystem != 0;
}

void Refresh() noexcept
{
    const auto playerTwo = reinterpret_cast<std::uintptr_t>(player_two::generation::PlayerTwoController());
    if (g_endUISystem == 0 || playerTwo == 0)
    {
        return;
    }
    try
    {
        // Player 2's component: its row widgets and request bytes.
        std::uintptr_t component = 0;
        Read(playerTwo + kLeaderActions, component);
        std::array<const UObject*, kComponentRows> named{};
        std::array<std::uint8_t, kComponentRows>   bytes{};
        for (std::size_t i = 0; component != 0 && i < kComponentRows; ++i)
        {
            std::uint64_t weak = 0;
            Read(component + kComponentRowBytes + kRowStride * i, bytes[i]);
            named[i] = Read(component + kComponentRowWeak + kRowStride * i, weak) ? Resolve(weak) : nullptr;
        }
        // Player 2's driven chocobo's own handles (Dash, Dismount, Jet, Scent, Scour): its tick makes their rows in the
        // one shared list, so they are player 2's the same way (RE-p2-chocobo-rows.md §4).
        constexpr std::size_t kRideRows = std::size(player_two::facts::kChocoboMoveStateRowHandles);
        std::array<const UObject*, kRideRows> rideNamed{};
        const auto ride = reinterpret_cast<std::uintptr_t>(player_two::chocobo_ride::DrivenMoveState());
        for (std::size_t i = 0; ride != 0 && i < kRideRows; ++i)
        {
            std::uint64_t weak = 0;
            rideNamed[i] = Read(ride + player_two::facts::kChocoboMoveStateRowHandles[i], weak) ? Resolve(weak) : nullptr;
        }
        // Player 2's field prompts (pickups): FieldActionMgr's slot rows in player 2's copy of its slot block, made by
        // player 2's pass of the game's own update (player_two/field_pickup.h).
        player_two::field_pickup::Weaks pickupWeaks{};
        player_two::field_pickup::RowWeaks(pickupWeaks);
        std::array<const UObject*, std::tuple_size_v<player_two::field_pickup::Weaks>> pickupNamed{};
        for (std::size_t i = 0; i < pickupWeaks.size(); ++i)
        {
            pickupNamed[i] = pickupWeaks[i] != 0 ? Resolve(pickupWeaks[i]) : nullptr;
        }

        std::uintptr_t list = 0;
        std::uintptr_t data = 0;
        std::int32_t   num  = 0;
        g_rows.clear();
        if (!Read(g_endUISystem + kRowList, list) || list == 0 || !Read(list, data) || !Read(list + kListNum, num) ||
            num < 0 || num > kMaxRows)
        {
            return;
        }
        g_stillTwo.clear();
        for (std::int32_t i = 0; i < num; ++i)
        {
            Row row;
            if (!Read(data + sizeof(std::uint64_t) * static_cast<std::size_t>(i), row.weak) ||
                (row.widget = Resolve(row.weak)) == nullptr)
            {
                continue;
            }
            for (std::size_t r = 0; r < kComponentRows; ++r)
            {
                row.p2Row = named[r] == row.widget ? static_cast<int>(r) : row.p2Row;
            }
            for (std::size_t r = 0; r < kRideRows; ++r)
            {
                row.p2Ride = rideNamed[r] != nullptr && rideNamed[r] == row.widget ? static_cast<int>(r) : row.p2Ride;
            }
            for (std::size_t r = 0; r < pickupNamed.size(); ++r)
            {
                row.p2Pickup = pickupNamed[r] != nullptr && pickupNamed[r] == row.widget ? static_cast<int>(r) : row.p2Pickup;
            }
            row.playerTwo = row.p2Row >= 0 || row.p2Ride >= 0 || row.p2Pickup >= 0 ||
                            std::find(g_playerTwoWidgets.begin(), g_playerTwoWidgets.end(), row.widget) != g_playerTwoWidgets.end();
            if (row.playerTwo)
            {
                g_stillTwo.push_back(row.widget);
            }
            const auto address = reinterpret_cast<std::uintptr_t>(row.widget);
            Read(address + kWidgetPriority, row.priority);
            Read(address + kWidgetRemoveState, row.remove);
            g_rows.push_back(row);
        }
        g_playerTwoWidgets.swap(g_stillTwo);
        LayOutPerPlayer();
        LogIfChanged(bytes);
    }
    catch (...)
    {
        g_rows.clear();
    }
}

void Forget() noexcept
{
    Restore();
    g_rows.clear();
    g_playerTwoWidgets.clear();  // a later row at a reused address must not count as player 2's
    g_laidOut.clear();
    g_rowLog.Reset();
}

void LayOutAgain(float width, float height) noexcept
{
    g_laidOut.clear();
    g_layOutAgain = true;
    g_againWidth  = width;
    g_againHeight = height;
}

void ShowOnly(int player) noexcept
{
    Restore();
    if (g_setOpacity == nullptr && !widget_opacity::Installed())
    {
        return;
    }
    try
    {
        for (const Row& row : g_rows)
        {
            UObject* widget = Resolve(row.weak);
            if (widget == nullptr || row.playerTwo == (player != 0) || !FindOffsets(widget))
            {
                continue;
            }
            const std::uint8_t visibility = VisibilityOf(widget);
            const float        previous   = OpacityOf(widget);
            if (visibility == kHidden || visibility == kCollapsed || previous == 0.0f)
            {
                continue;  // not drawn anyway
            }
            if (SetOpacity(widget, 0.0f))
            {
                g_hidden.push_back({row.weak, previous});
            }
        }
    }
    catch (...)
    {
    }
}

void Restore() noexcept
{
    try
    {
        for (const Hidden& hidden : g_hidden)
        {
            if (UObject* widget = Resolve(hidden.weak))
            {
                SetOpacity(widget, hidden.previous);
            }
        }
    }
    catch (...)
    {
    }
    g_hidden.clear();
}

}  // namespace lss::hud::prompt_rows
