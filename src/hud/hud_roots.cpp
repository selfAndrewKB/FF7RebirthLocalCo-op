#include "hud/hud_roots.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "hud/user_widgets.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/scopes/primary_controller.h"
#include "players/local_players.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lss::hud::hud_roots {

namespace {

using RC::Unreal::UObject;

// UUserWidget::SetPlayerContext(widget, const FLocalPlayerContext&): writes the 16 bytes at +0x248
// (weak local player, weak world) and walks the widget's child user widgets. No events.
constexpr std::uint8_t   kSetPlayerContextPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC,
                                                        0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSetPlayerContext{L"UUserWidget::SetPlayerContext", 0x140F59C50,
                                                   kSetPlayerContextPrologue};
constexpr std::uint32_t  kWidgetWorldWeak = 0x250;

using SetPlayerContextFn = void (*)(void* widget, const void* context);

struct alignas(8) PlayerContext
{
    std::uint64_t localPlayer = 0;  // FWeakObjectPtr
    std::uint64_t world       = 0;
};

using Root = user_widgets::Widget;  // an object followed by pointer, GUObjectArray index and class

SetPlayerContextFn g_setContext = nullptr;  // set once at install

// ESlateVisibility: Collapsed and Hidden widgets are neither ticked nor painted.
constexpr std::uint8_t kCollapsed = 1;
constexpr std::uint8_t kHidden    = 2;
constexpr std::int32_t kNoOffset  = -1;

// Game thread.
std::vector<Root> g_roots;
std::vector<char> g_swapped;  // per root: given a further seat's context in this pass (put back after)
std::vector<char> g_selfPlacing;  // per root: shows or collapses itself in each run (marked per frame)
std::size_t       g_hiddenSwapped = 0;  // self-placing roots given a seat's context while collapsed, last pass
PlayerContext     g_contexts[players::kMaxPlayers];  // by seat; a seat with no local player has none
Root              g_gameInstance;  // found once: FindFirstOf walks every UObject
std::int32_t      g_slotOffset       = kNoOffset;  // UWidget::Slot and UWidget::Visibility, found once
std::int32_t      g_visibilityOffset = kNoOffset;
std::unordered_map<const void*, bool> g_ownedByGameInstance;  // per outer class

bool SetContextGuarded(void* widget, const PlayerContext& context) noexcept
{
    __try
    {
        g_setContext(widget, &context);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// UWidget's `Slot` (a pointer) and `Visibility` (one byte) offsets, read by reflection once.
bool FindOffsets(UObject* widget)
{
    if (g_slotOffset != kNoOffset)
    {
        return true;
    }
    auto* slot       = widget->GetPropertyByNameInChain(STR("Slot"));
    auto* visibility = widget->GetPropertyByNameInChain(STR("Visibility"));
    if (slot == nullptr || slot->GetSize() != static_cast<std::int32_t>(sizeof(UObject*)) || visibility == nullptr ||
        visibility->GetSize() != 1)
    {
        return false;
    }
    g_slotOffset       = slot->GetOffset_Internal();
    g_visibilityOffset = visibility->GetOffset_Internal();
    return true;
}

/// A HUD root: a UUserWidget on the overlay itself (no slot), owned by the game instance.
bool IsViewportRoot(UObject* widget)
{
    UObject* slot = nullptr;
    if (!FindOffsets(widget) ||
        !platform::CopyGuarded(&slot, reinterpret_cast<const std::uint8_t*>(widget) + g_slotOffset, sizeof(slot)) ||
        slot != nullptr)
    {
        return false;
    }
    UObject* outer = widget->GetOuterPrivate();
    if (outer == nullptr)
    {
        return false;
    }
    const void* outerClass = outer->GetClassPrivate();
    if (const auto found = g_ownedByGameInstance.find(outerClass); found != g_ownedByGameInstance.end())
    {
        return found->second;
    }
    const bool owned = std::wstring(outer->GetClassPrivate()->GetName()).find(L"GameInstance") != std::wstring::npos;
    g_ownedByGameInstance.emplace(outerClass, owned);
    return owned;
}

/// Whether the root is ticked and painted (not Collapsed or Hidden). Unknown counts as shown.
bool Shown(const Root& root) noexcept
{
    std::uint8_t visibility = 0;
    return g_visibilityOffset == kNoOffset ||
           !platform::CopyGuarded(&visibility, reinterpret_cast<const std::uint8_t*>(root.object) + g_visibilityOffset,
                                  sizeof(visibility)) ||
           (visibility != kCollapsed && visibility != kHidden);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto context = platform::FindFunction(image, kSetPlayerContext);
    if (!context)
    {
        return false;
    }
    g_setContext = reinterpret_cast<SetPlayerContextFn>(*context);
    return true;
}

bool LocalPlayers(LocalPlayerSet& bySeat)
{
    bySeat = {};
    if (g_gameInstance.object == nullptr || !user_widgets::Alive(g_gameInstance))
    {
        UObject* instance = players::unreal::GameInstance();
        g_gameInstance    = instance == nullptr ? Root{}
                                                : Root{instance, static_cast<std::int32_t>(instance->GetInternalIndex()),
                                                       instance->GetClassPrivate()};
    }
    auto* list = g_gameInstance.object == nullptr ? nullptr : players::unreal::LocalPlayers(g_gameInstance.object);
    for (std::int32_t i = 0; list != nullptr && i < list->Num(); ++i)
    {
        // By the controller id, which is the seat: the list's order is the views' on screen, not the seats'.
        const std::int32_t id = players::LocalPlayerControllerId((*list)[i]);
        if (id >= 0 && id < players::kMaxPlayers && bySeat[static_cast<std::size_t>(id)] == nullptr)
        {
            bySeat[static_cast<std::size_t>(id)] = (*list)[i];
        }
    }
    static bool s_said = false;
    if (bySeat[0] == nullptr && list != nullptr && list->Num() > 0 && !s_said)
    {
        s_said = true;
        platform::Log(L"HUD roots: no listed local player has controller id 0 (player 1's); the further players' HUD "
                      L"content stays off while that holds (logged once)");
    }
    return bySeat[0] != nullptr;
}

bool Scan(const LocalPlayerSet& bySeat)
{
    // Every root is back to player 1's context between frames, so nothing is lost by forgetting
    // which were swapped.
    g_roots.clear();
    for (const auto& live : user_widgets::Live())
    {
        if (IsViewportRoot(live.object))
        {
            g_roots.push_back({live.object, live.index, live.cls});
        }
    }
    // The world half of each context is the one the roots already carry (they are all player 1's);
    // the player half is each local player's weak pointer.
    std::uint64_t world = 0;
    for (const Root& root : g_roots)
    {
        if (platform::CopyGuarded(&world, reinterpret_cast<const std::uint8_t*>(root.object) + kWidgetWorldWeak,
                                  sizeof(world)) &&
            world != 0)
        {
            break;
        }
    }
    g_swapped.assign(g_roots.size(), 0);
    g_selfPlacing.assign(g_roots.size(), 0);
    for (std::size_t seat = 0; seat < bySeat.size(); ++seat)
    {
        const std::uint64_t weak = bySeat[seat] == nullptr ? 0 : player_two::primary_controller::WeakValueOf(bySeat[seat]);
        g_contexts[seat]         = weak == 0 ? PlayerContext{} : PlayerContext{weak, world};
    }
    return world != 0 && g_contexts[0].localPlayer != 0;
}

bool HasContext(int seat) noexcept
{
    return seat >= 0 && seat < players::kMaxPlayers && g_contexts[seat].localPlayer != 0;
}

void Adopt(UObject* widget)
{
    if (widget == nullptr ||
        std::any_of(g_roots.begin(), g_roots.end(), [widget](const Root& root) { return root.object == widget; }) ||
        !IsViewportRoot(widget))
    {
        return;
    }
    g_roots.push_back({widget, static_cast<std::int32_t>(widget->GetInternalIndex()), widget->GetClassPrivate()});
    g_swapped.push_back(0);
    g_selfPlacing.push_back(0);
}

bool AdoptSelfPlacing(UObject* widget)
{
    Adopt(widget);
    for (std::size_t i = 0; widget != nullptr && i < g_roots.size() && i < g_selfPlacing.size(); ++i)
    {
        if (g_roots[i].object == widget)
        {
            g_selfPlacing[i] = 1;
            return true;
        }
    }
    return false;
}

bool SetContexts(int seat) noexcept
{
    // A further seat's context goes only to roots that are shown this frame (a hidden root is neither
    // ticked nor painted), and to the self-placing ones whatever their visibility (their run shows
    // or collapses them from the context's own projection); player 1's goes back to exactly those.
    const bool toTwo = seat != 0;  // toward a further seat
    if (!HasContext(seat))
    {
        return false;
    }
    const PlayerContext& context = g_contexts[seat];
    bool                 all     = true;
    if (toTwo)
    {
        g_hiddenSwapped = 0;
    }
    for (std::size_t i = 0; i < g_roots.size() && i < g_swapped.size() && i < g_selfPlacing.size(); ++i)
    {
        const Root& root  = g_roots[i];
        const bool  shown = !toTwo || Shown(root);
        if (toTwo ? !shown && g_selfPlacing[i] == 0 : g_swapped[i] == 0)
        {
            continue;
        }
        g_hiddenSwapped += toTwo && !shown ? 1 : 0;
        g_swapped[i] = toTwo ? 1 : 0;
        if (user_widgets::Alive(root) && !SetContextGuarded(root.object, context))
        {
            all = false;
            if (toTwo)
            {
                return false;  // the caller puts player 1's back on every root swapped so far
            }
        }
    }
    if (!toTwo)
    {
        std::fill(g_selfPlacing.begin(), g_selfPlacing.end(), char{0});  // marked again each frame
    }
    return all;
}

std::size_t Count() noexcept
{
    return g_roots.size();
}

std::size_t HiddenSwapped() noexcept
{
    return g_hiddenSwapped;
}

}  // namespace lss::hud::hud_roots
