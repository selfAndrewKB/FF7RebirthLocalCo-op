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
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lss::hud::hud_roots {

namespace {

using RC::Unreal::UObject;

// UUserWidget::SetPlayerContext(widget, const FLocalPlayerContext&): writes the 16 bytes at +0x248
// (weak local player, weak world) and walks the widget's child user widgets. No events.
constexpr std::uintptr_t kSetPlayerContext = 0x140F59C50;
constexpr std::uint8_t   kSetPlayerContextPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC,
                                                        0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr std::uint32_t  kWidgetWorldWeak = 0x250;

using SetPlayerContextFn = void (*)(void* widget, const void* context);

struct alignas(8) PlayerContext
{
    std::uint64_t localPlayer = 0;  // FWeakObjectPtr
    std::uint64_t world       = 0;
};

using Root = user_widgets::Widget;  // an object followed by pointer, GUObjectArray index and class

SetPlayerContextFn g_setContext = nullptr;  // set once at install

// Game thread.
std::vector<Root> g_roots;
PlayerContext     g_contexts[2];
Root              g_gameInstance;  // found once: FindFirstOf walks every UObject

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

/// A HUD root: a UUserWidget on the overlay itself (no slot), owned by the game instance.
bool IsViewportRoot(UObject* widget)
{
    auto* slot = widget->GetPropertyByNameInChain(STR("Slot"));
    if (slot == nullptr || slot->GetSize() != static_cast<std::int32_t>(sizeof(UObject*)))
    {
        return false;
    }
    UObject** value = widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot"));
    if (value == nullptr || *value != nullptr)
    {
        return false;
    }
    UObject* outer = widget->GetOuterPrivate();
    return outer != nullptr && std::wstring(outer->GetClassPrivate()->GetName()).find(L"GameInstance") != std::wstring::npos;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto context = image.Rebase(kSetPlayerContext);
    if (!context || !image.BytesMatch(kSetPlayerContext, kSetPlayerContextPrologue))
    {
        return false;
    }
    g_setContext = reinterpret_cast<SetPlayerContextFn>(*context);
    return true;
}

bool LocalPlayers(UObject*& one, UObject*& two)
{
    if (g_gameInstance.object == nullptr || !user_widgets::Alive(g_gameInstance))
    {
        UObject* instance = players::unreal::GameInstance();
        g_gameInstance    = instance == nullptr ? Root{}
                                                : Root{instance, static_cast<std::int32_t>(instance->GetInternalIndex()),
                                                       instance->GetClassPrivate()};
    }
    auto* list = g_gameInstance.object == nullptr ? nullptr : players::unreal::LocalPlayers(g_gameInstance.object);
    if (list == nullptr || list->Num() < 2)
    {
        return false;
    }
    one = (*list)[0];
    two = (*list)[1];
    return true;
}

bool Scan(UObject* playerOne, UObject* playerTwo)
{
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
    g_contexts[0] = {player_two::primary_controller::WeakValueOf(playerOne), world};
    g_contexts[1] = {player_two::primary_controller::WeakValueOf(playerTwo), world};
    return world != 0 && g_contexts[0].localPlayer != 0 && g_contexts[1].localPlayer != 0;
}

bool SetContexts(int player) noexcept
{
    const PlayerContext& context = g_contexts[player == 0 ? 0 : 1];
    for (const Root& root : g_roots)
    {
        if (user_widgets::Alive(root) && !SetContextGuarded(root.object, context))
        {
            return false;
        }
    }
    return true;
}

std::size_t Count() noexcept
{
    return g_roots.size();
}

}  // namespace lss::hud::hud_roots
