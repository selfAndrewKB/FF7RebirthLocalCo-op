#include "hud/hud_roots.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "hud/context_owners.h"
#include "hud/user_widgets.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/scopes/primary_controller.h"
#include "players/local_players.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <cstdint>
#include <format>
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

using context_owners::Owner;

/// What the setter costs per root class (the census line): a class can have many roots (markers).
struct ClassCost
{
    std::wstring  name;
    std::uint64_t calls   = 0;
    double        seconds = 0.0;
};

// Game thread.
std::vector<Root>          g_roots;
std::vector<Owner>         g_owner;  // per root: the seat whose context it carries (hud/context_owners.h)
std::vector<char>          g_selfPlacing;  // per root: shows or collapses itself in each run (marked per frame)
std::vector<std::uint16_t> g_costOf;  // per root: its class's entry in g_costs
std::vector<ClassCost>     g_costs;  // by class, in the order first seen; kept across scans
std::unordered_map<const void*, std::uint16_t> g_costIndex;  // class -> entry in g_costs
std::uint64_t              g_setsToSeats    = 0;  // since the last census: setter calls toward a further seat
std::uint64_t              g_setsToOne      = 0;  // and back to player 1
std::uint64_t              g_alreadyOwned   = 0;  // a pass needed the root, which carried that seat's context already
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

/// The root's class entry in the census, made on first sight (the class name read then, every scan at most).
std::uint16_t CostEntry(const Root& root)
{
    if (const auto found = g_costIndex.find(root.cls); found != g_costIndex.end())
    {
        return found->second;
    }
    const auto at = static_cast<std::uint16_t>(std::min<std::size_t>(g_costs.size(), 0xFFFF));
    if (at == 0xFFFF)
    {
        return at;  // past the table: uncounted (not reached: the HUD has some hundred classes)
    }
    g_costs.push_back({std::wstring(root.object->GetClassPrivate()->GetName())});
    g_costIndex.emplace(root.cls, at);
    return at;
}

/// One root's setter call, timed for the census.
bool SetTimed(std::size_t i, const PlayerContext& context) noexcept
{
    const double start = platform::frame_watch::Now();
    const bool   set   = SetContextGuarded(g_roots[i].object, context);
    if (i < g_costOf.size() && g_costOf[i] < g_costs.size())
    {
        ClassCost& cost = g_costs[g_costOf[i]];
        ++cost.calls;
        cost.seconds += platform::frame_watch::Now() - start;
    }
    return set;
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
    // Every root is back to player 1's context between frames, and a scan runs before the frame's first switch, so
    // no root carries a further seat's here. Defensive: one that did is put back before the list is forgotten (the
    // result is the way back's own; a fault there is not retried).
    if (std::any_of(g_owner.begin(), g_owner.end(), [](Owner owner) { return owner != 0; }))
    {
        SetContexts(0);
    }
    g_roots.clear();
    g_costOf.clear();
    for (const auto& live : user_widgets::Live())
    {
        if (IsViewportRoot(live.object))
        {
            const Root root{live.object, live.index, live.cls};
            const auto cost = CostEntry(root);  // allocates: before the lists grow, so they stay in step
            g_roots.push_back(root);
            g_costOf.push_back(cost);
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
    g_owner.assign(g_roots.size(), 0);
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
    const Root root{widget, static_cast<std::int32_t>(widget->GetInternalIndex()), widget->GetClassPrivate()};
    const auto cost = CostEntry(root);  // allocates: before the lists grow, so they stay in step
    g_roots.push_back(root);
    g_owner.push_back(0);  // made with player 1's context, as every root
    g_selfPlacing.push_back(0);
    g_costOf.push_back(cost);
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
    // A further seat's context goes only to roots that are shown in its pass (a hidden root is neither
    // ticked nor painted), and to the self-placing ones whatever their visibility (their run shows
    // or collapses them from the context's own projection), each only if it does not carry the seat's
    // already; player 1's goes back to every root a seat's reached (hud/context_owners.h).
    const bool toSeat = seat != 0;  // toward a further seat
    if (!HasContext(seat))
    {
        return false;
    }
    const PlayerContext& context = g_contexts[seat];
    bool                 all     = true;
    if (toSeat)
    {
        g_hiddenSwapped = 0;
    }
    for (std::size_t i = 0; i < g_roots.size() && i < g_owner.size() && i < g_selfPlacing.size(); ++i)
    {
        const Root& root   = g_roots[i];
        const bool  shown  = toSeat && Shown(root);
        const bool  needed = shown || g_selfPlacing[i] != 0;
        const bool set = toSeat ? context_owners::SetForSeat(g_owner[i], seat, needed)
                                : context_owners::SetForPlayerOne(g_owner[i]);
        if (!set)
        {
            g_alreadyOwned += toSeat && needed ? 1 : 0;
            continue;
        }
        g_hiddenSwapped += toSeat && !shown ? 1 : 0;
        g_owner[i] = static_cast<Owner>(seat);  // before the call: a fault leaves it to the way back
        if (!user_widgets::Alive(root))
        {
            continue;
        }
        (toSeat ? g_setsToSeats : g_setsToOne) += 1;
        if (!SetTimed(i, context))
        {
            all = false;
            if (toSeat)
            {
                return false;  // the caller puts player 1's back on every root a seat's reached
            }
        }
    }
    if (!toSeat)
    {
        std::fill(g_selfPlacing.begin(), g_selfPlacing.end(), char{0});  // marked again each frame
    }
    return all;
}

std::wstring TakeCensus(std::uint32_t frames)
{
    const double perFrame = frames == 0 ? 0.0 : 1.0 / frames;
    double       total    = 0.0;
    for (const ClassCost& cost : g_costs)
    {
        total += cost.seconds;
    }
    std::vector<const ClassCost*> costliest;
    for (const ClassCost& cost : g_costs)
    {
        if (cost.calls != 0)
        {
            costliest.push_back(&cost);
        }
    }
    constexpr std::size_t kShown = 8;
    const std::size_t     shown  = std::min(costliest.size(), kShown);
    std::partial_sort(costliest.begin(), costliest.begin() + static_cast<std::ptrdiff_t>(shown), costliest.end(),
                      [](const ClassCost* a, const ClassCost* b) { return a->seconds > b->seconds; });
    std::wstring line = std::format(L"{} root(s), {} root class(es) seen; per frame {:.1f} call(s) toward the further "
                                    L"players, {:.1f} back to player 1, {:.1f} needed but already that player's; the "
                                    L"setter {:.3f} ms per frame; the costliest:",
                                    g_roots.size(), g_costs.size(), g_setsToSeats * perFrame, g_setsToOne * perFrame,
                                    g_alreadyOwned * perFrame, total * 1000.0 * perFrame);
    for (std::size_t i = 0; i < shown; ++i)
    {
        line += std::format(L"{} {} {:.3f} ms ({:.1f} call(s))", i == 0 ? L"" : L",", costliest[i]->name,
                            costliest[i]->seconds * 1000.0 * perFrame, costliest[i]->calls * perFrame);
    }
    ResetCensus();
    return line;
}

void ResetCensus() noexcept
{
    for (ClassCost& cost : g_costs)
    {
        cost.calls   = 0;
        cost.seconds = 0.0;
    }
    g_setsToSeats = g_setsToOne = g_alreadyOwned = 0;
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
