#include "hud/prompt_rows.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
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
    bool           playerTwo = false;
    std::int32_t   priority  = 0;
    std::int32_t   remove    = 0;
    bool operator==(const Row&) const = default;
};

struct Hidden
{
    std::uint64_t weak     = 0;
    std::uint8_t  previous = 0;
};

// Set once at install.
std::uintptr_t  g_endUISystem   = 0;
SortAndLayoutFn g_sortAndLayout = nullptr;
UFunction*      g_setVisibility = nullptr;
std::int32_t    g_visibilityOffset = -1;

// Game thread.
std::vector<Row>                          g_rows;
std::vector<Row>                          g_logged;
std::array<std::uint8_t, kComponentRows>  g_loggedBytes{};
std::vector<const UObject*>               g_playerTwoWidgets;  // player 2's until they leave the list
std::vector<std::pair<const UObject*, bool>> g_laidOut;        // the list as last laid out per player
std::vector<Hidden>                       g_hidden;
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

bool LayoutGuarded(ListHeader* list) noexcept
{
    __try
    {
        g_sortAndLayout(list, nullptr);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Each player's rows laid out on their own, when the list or an owner changed.
void LayOutPerPlayer()
{
    std::vector<std::pair<const UObject*, bool>> now;
    for (const Row& row : g_rows)
    {
        now.emplace_back(row.widget, row.playerTwo);
    }
    if (now == g_laidOut || g_sortAndLayout == nullptr)
    {
        return;
    }
    g_laidOut = std::move(now);
    for (const bool playerTwo : {false, true})
    {
        std::vector<std::uint64_t> weaks;
        for (const Row& row : g_rows)
        {
            if (row.playerTwo == playerTwo)
            {
                weaks.push_back(row.weak);
            }
        }
        ListHeader list{weaks.data(), static_cast<std::int32_t>(weaks.size()), static_cast<std::int32_t>(weaks.size())};
        if (!weaks.empty() && !LayoutGuarded(&list))
        {
            g_sortAndLayout = nullptr;
            platform::Log(L"prompt rows: the game's layout raised an exception; rows keep the shared layout");
            return;
        }
    }
}

std::uint8_t VisibilityOf(const UObject* widget) noexcept
{
    std::uint8_t visibility = 0;
    Read(reinterpret_cast<std::uintptr_t>(widget) + static_cast<std::uint32_t>(g_visibilityOffset), visibility);
    return visibility;
}

bool SetVisibility(UObject* widget, std::uint8_t visibility)
{
    struct
    {
        std::uint8_t visibility;
    } params{visibility};
    return g_setVisibility != nullptr &&
           players::unreal::GuardedProcessEvent(widget, g_setVisibility, &params);
}

bool FindVisibilityOffset(UObject* widget)
{
    if (g_visibilityOffset >= 0)
    {
        return true;
    }
    auto* property = widget->GetPropertyByNameInChain(STR("Visibility"));
    if (property == nullptr || property->GetSize() != 1)
    {
        return false;
    }
    g_visibilityOffset = property->GetOffset_Internal();
    return true;
}

std::wstring Describe(const std::vector<Row>& rows)
{
    std::wstring text;
    for (const Row& row : rows)
    {
        const std::wstring owner = row.p2Row >= 0   ? std::format(L"player 2 row {}", row.p2Row)
                                   : row.playerTwo ? std::wstring(L"player 2 (leaving)")
                                                   : std::wstring(L"player 1");
        text += std::format(L"{}[{} prio {} remove {}]", text.empty() ? L"" : L" ", owner, row.priority, row.remove);
    }
    return text.empty() ? L"none" : text;
}

void LogIfChanged(const std::array<std::uint8_t, kComponentRows>& bytes)
{
    if (g_lines >= kMaxLines || (g_rows == g_logged && bytes == g_loggedBytes))
    {
        return;
    }
    std::wstring requests;
    for (std::size_t i = 0; i < kComponentRows; ++i)
    {
        requests += bytes[i] != 0 ? std::format(L" {}:{:#04x}", i, bytes[i]) : L"";
    }
    ++g_lines;
    platform::Log(L"prompt rows: {}; player 2's requests:{}", Describe(g_rows), requests.empty() ? L" none" : requests);
    g_logged      = g_rows;
    g_loggedBytes = bytes;
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
        g_setVisibility = RC::Unreal::UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                                                    STR("/Script/UMG.Widget:SetVisibility"));
    }
    catch (...)
    {
        g_setVisibility = nullptr;
    }
    platform::Log(L"prompt rows: per-player layout {}, per-half visibility {}", g_sortAndLayout ? L"ready" : L"UNAVAILABLE",
                  g_setVisibility ? L"ready" : L"UNAVAILABLE");
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

        std::uintptr_t list = 0;
        std::uintptr_t data = 0;
        std::int32_t   num  = 0;
        g_rows.clear();
        if (!Read(g_endUISystem + kRowList, list) || list == 0 || !Read(list, data) || !Read(list + kListNum, num) ||
            num < 0 || num > kMaxRows)
        {
            return;
        }
        std::vector<const UObject*> stillTwo;
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
            row.playerTwo = row.p2Row >= 0 || std::find(g_playerTwoWidgets.begin(), g_playerTwoWidgets.end(),
                                                        row.widget) != g_playerTwoWidgets.end();
            if (row.playerTwo)
            {
                stillTwo.push_back(row.widget);
            }
            const auto address = reinterpret_cast<std::uintptr_t>(row.widget);
            Read(address + kWidgetPriority, row.priority);
            Read(address + kWidgetRemoveState, row.remove);
            g_rows.push_back(row);
        }
        g_playerTwoWidgets = std::move(stillTwo);
        LayOutPerPlayer();
        LogIfChanged(bytes);
    }
    catch (...)
    {
        g_rows.clear();
    }
}

void ShowOnly(int player) noexcept
{
    Restore();
    if (g_setVisibility == nullptr)
    {
        return;
    }
    try
    {
        for (const Row& row : g_rows)
        {
            UObject* widget = Resolve(row.weak);
            if (widget == nullptr || row.playerTwo == (player != 0) || !FindVisibilityOffset(widget))
            {
                continue;
            }
            const std::uint8_t previous = VisibilityOf(widget);
            if (previous == kHidden || previous == kCollapsed)
            {
                continue;
            }
            if (SetVisibility(widget, kHidden))
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
                SetVisibility(widget, hidden.previous);
            }
        }
    }
    catch (...)
    {
    }
    g_hidden.clear();
}

}  // namespace lss::hud::prompt_rows
