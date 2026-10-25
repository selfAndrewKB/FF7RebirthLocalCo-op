#include "hud/user_widgets.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/frame_watch.h"
#include "platform/log.h"
#include "players/object_slot.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace lss::hud::user_widgets {

namespace {

using RC::Unreal::UClass;
using RC::Unreal::UObject;
namespace Hook = RC::Unreal::Hook;

UClass*                 g_userWidgetClass = nullptr;
// UE4SS c0a8d4c keeps a construction callback for the session (it has no unregister), so it is registered once and
// made inert by Remove; the DLL is pinned, so its code stays.
bool                    g_registered = false;  // game thread
std::atomic<bool>       g_active{false};       // read on the constructing thread
std::mutex              g_lock;
std::vector<Widget>     g_constructed;  // under g_lock
std::vector<Widget>     g_live;         // game thread
bool                    g_seeded = false;

Widget Record(UObject* object) noexcept
{
    return {object, static_cast<std::int32_t>(object->GetInternalIndex()), object->GetClassPrivate()};
}

bool g_sorted = true;  // g_live is sorted by index with no duplicates

void Seed()
{
    g_seeded = true;
    const platform::frame_watch::Job job(L"UObject walk (user widgets)");
    std::vector<UObject*> found;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("UserWidget"), found);
    g_live.clear();
    for (UObject* object : found)
    {
        g_live.push_back(Record(object));
    }
    g_sorted = false;
}

}  // namespace

bool Install() noexcept
{
    try
    {
        g_userWidgetClass = RC::Unreal::UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.UserWidget"));
        if (g_userWidgetClass == nullptr)
        {
            platform::Log(L"user widgets: /Script/UMG.UserWidget not found; HUD widgets are found by scanning instead");
            return false;
        }
        if (!g_registered)
        {
            Hook::RegisterStaticConstructObjectPostCallback(
                [](const RC::Unreal::FStaticConstructObjectParameters&, UObject* constructed) -> UObject* {
                    if (g_active.load(std::memory_order_acquire) && constructed != nullptr &&
                        constructed->IsA(g_userWidgetClass))
                    {
                        const std::lock_guard lock(g_lock);
                        g_constructed.push_back(Record(constructed));
                    }
                    return constructed;  // the game's object, unchanged (the last callback's answer is the one kept)
                });
            g_registered = true;
        }
        g_active.store(true, std::memory_order_release);
        // The widgets made before the callback: one walk now, while the game loads, not at the
        // first join (it costs ~40 ms in play).
        Seed();
        return true;
    }
    catch (...)
    {
        platform::Log(L"user widgets: the construction callback could not be registered");
        return false;
    }
}

void Remove() noexcept
{
    g_active.store(false, std::memory_order_release);
}

bool Alive(const Widget& widget) noexcept
{
    return players::LiveInSlot(widget.index, widget.object) && widget.object->GetClassPrivate() == widget.cls;
}

const std::vector<Widget>& Live() noexcept
{
    try
    {
        if (!g_seeded || !g_active.load(std::memory_order_acquire))
        {
            Seed();  // once (or every time, without the callback): the widgets constructed before
        }
        {
            const std::lock_guard lock(g_lock);
            if (!g_constructed.empty())
            {
                g_live.insert(g_live.end(), g_constructed.begin(), g_constructed.end());
                g_constructed.clear();
                g_sorted = false;
            }
        }
        std::erase_if(g_live, [](const Widget& widget) { return !Alive(widget); });
        if (!g_sorted)
        {
            // A widget recorded both by the walk and by the callback appears once.
            std::sort(g_live.begin(), g_live.end(), [](const Widget& a, const Widget& b) { return a.index < b.index; });
            g_live.erase(std::unique(g_live.begin(), g_live.end(),
                                     [](const Widget& a, const Widget& b) { return a.object == b.object; }),
                         g_live.end());
            g_sorted = true;
        }
    }
    catch (...)
    {
        platform::Log(L"user widgets: failed to update the list (an exception); kept as it was");
    }
    return g_live;
}

}  // namespace lss::hud::user_widgets
