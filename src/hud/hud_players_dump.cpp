#include "hud/hud_players_dump.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "hud/hud_trace.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

using RC::Unreal::FName;
using RC::Unreal::FProperty;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

constexpr std::size_t kFrame           = 64;   // parameter frame; every function used here needs less
constexpr int         kMaxRootLines    = 60;
constexpr int         kMaxSettingLines = 400;
constexpr int         kMaxControllers  = 4;

using players::unreal::GuardedProcessEvent;
using platform::CopyGuarded;

/// The offset of a parameter in `function`'s frame, if it exists with this size and fits.
std::optional<std::int32_t> ParamOffset(UFunction* function, const wchar_t* name, std::int32_t size) noexcept
{
    auto* property = function->FindProperty(FName(name, RC::Unreal::FNAME_Find));
    if (property == nullptr)
    {
        return std::nullopt;
    }
    const std::int32_t offset = property->GetOffset_Internal();
    if (property->GetSize() != size || offset < 0 || offset + size > function->GetParmsSize())
    {
        return std::nullopt;
    }
    return offset;
}

/// A reflected function returning an object, with no inputs, called on `target`.
struct ObjectGetter
{
    UFunction*   function = nullptr;
    std::int32_t result   = -1;

    /// The first of `paths` that names such a function (UMG declares some getters on UWidget and
    /// some on UUserWidget).
    static ObjectGetter Find(std::initializer_list<const wchar_t*> paths) noexcept
    {
        for (const wchar_t* path : paths)
        {
            auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
            if (function == nullptr || function->GetParmsSize() > static_cast<std::int32_t>(kFrame))
            {
                continue;
            }
            if (const auto result = ParamOffset(function, STR("ReturnValue"), sizeof(UObject*)))
            {
                return ObjectGetter{function, *result};
            }
        }
        return {};
    }

    bool Found() const noexcept { return function != nullptr; }

    std::optional<UObject*> Call(UObject* target) const noexcept
    {
        alignas(16) std::uint8_t frame[kFrame]{};
        if (!Found() || target == nullptr || !GuardedProcessEvent(target, function, frame))
        {
            return std::nullopt;
        }
        return *reinterpret_cast<UObject* const*>(frame + result);
    }
};

std::wstring NameOf(const UObject* object)
{
    return object == nullptr ? std::wstring(L"(none)") : std::wstring(object->GetName());
}

/// What `object` is to the co-op session: a player's controller or local player, or its name.
class Players
{
public:
    Players()
    {
        controllers_[0] = static_cast<UObject*>(player_two::generation::PlayerOneController());
        controllers_[1] = static_cast<UObject*>(player_two::generation::PlayerTwoController());
        if (auto* list = players::unreal::LocalPlayers(players::unreal::GameInstance()))
        {
            for (std::int32_t i = 0; i < list->Num(); ++i)
            {
                localPlayers_.push_back((*list)[i]);
            }
        }
    }

    const std::vector<UObject*>& LocalPlayers() const noexcept { return localPlayers_; }

    std::wstring Label(const UObject* object) const
    {
        if (object == nullptr)
        {
            return L"(none)";
        }
        for (int i = 0; i < 2; ++i)
        {
            if (object == controllers_[i])
            {
                return std::format(L"player {}'s controller", i + 1);
            }
        }
        for (std::size_t i = 0; i < localPlayers_.size(); ++i)
        {
            if (object == localPlayers_[i])
            {
                return std::format(L"LocalPlayers[{}]", i);
            }
        }
        return NameOf(object);
    }

private:
    UObject*              controllers_[2] = {};
    std::vector<UObject*> localPlayers_;
};

int LogLocalPlayers(const Players& players)
{
    int lines = 0;
    for (std::size_t i = 0; i < players.LocalPlayers().size(); ++i)
    {
        UObject*  localPlayer = players.LocalPlayers()[i];
        UObject** controller  = players::unreal::ControllerField(localPlayer);
        platform::Log(L"hud players: LocalPlayers[{}] {} -> {}", i, NameOf(localPlayer),
                      controller != nullptr ? players.Label(*controller) : std::wstring(L"(controller field not found)"));
        ++lines;
    }
    return lines;
}

/// UGameplayStatics::GetPlayerController(context, index) walks the world's controller list in order.
int LogControllerOrder(const Players& players)
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                                  STR("/Script/Engine.GameplayStatics:GetPlayerController"));
    auto* statics = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics"));
    UObject* context = players::unreal::GameInstance();
    if (function == nullptr || statics == nullptr || context == nullptr ||
        function->GetParmsSize() > static_cast<std::int32_t>(kFrame))
    {
        platform::Log(L"hud players: GameplayStatics:GetPlayerController not found; controller order not read");
        return 1;
    }
    const auto contextAt = ParamOffset(function, STR("WorldContextObject"), sizeof(UObject*));
    const auto indexAt   = ParamOffset(function, STR("PlayerIndex"), sizeof(std::int32_t));
    const auto resultAt  = ParamOffset(function, STR("ReturnValue"), sizeof(UObject*));
    if (!contextAt || !indexAt || !resultAt)
    {
        platform::Log(L"hud players: GetPlayerController's parameters are not as expected; controller order not read");
        return 1;
    }
    std::wstring order;
    for (std::int32_t i = 0; i < kMaxControllers; ++i)
    {
        alignas(16) std::uint8_t frame[kFrame]{};
        *reinterpret_cast<UObject**>(frame + *contextAt)    = context;
        *reinterpret_cast<std::int32_t*>(frame + *indexAt) = i;
        if (!GuardedProcessEvent(statics->GetClassDefaultObject(), function, frame))
        {
            order += std::format(L" [{}] (call failed)", i);
            break;
        }
        UObject* controller = *reinterpret_cast<UObject* const*>(frame + *resultAt);
        if (controller == nullptr)
        {
            break;
        }
        order += std::format(L" [{}] {}", i, players.Label(controller));
    }
    platform::Log(L"hud players: the world's controller list:{}", order.empty() ? L" (empty)" : order);
    return 1;
}

int LogPrimaryController(const Players& players)
{
    if (!player_two::primary_controller::Installed())
    {
        platform::Log(L"hud players: primary controller not readable (the party manager was not found at startup)");
        return 1;
    }
    platform::Log(L"hud players: the party manager's primary controller: {}",
                  players.Label(static_cast<UObject*>(player_two::primary_controller::Current())));
    return 1;
}

bool IsViewportRoot(UObject* widget)
{
    auto* slot = widget->GetPropertyByNameInChain(STR("Slot"));
    if (slot == nullptr || slot->GetSize() != static_cast<std::int32_t>(sizeof(UObject*)) ||
        *widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")) != nullptr)
    {
        return false;  // inside a panel: its root decides where it goes
    }
    UObject* outer = widget->GetOuterPrivate();
    return outer != nullptr && std::wstring(outer->GetClassPrivate()->GetName()).find(L"GameInstance") != std::wstring::npos;
}

/// Each HUD root (no slot, owned by the game instance): its owning player and local player.
int LogRoots(const Players& players)
{
    const auto owningPlayer      = ObjectGetter::Find({STR("/Script/UMG.Widget:GetOwningPlayer"),
                                                       STR("/Script/UMG.UserWidget:GetOwningPlayer")});
    const auto owningLocalPlayer = ObjectGetter::Find({STR("/Script/UMG.Widget:GetOwningLocalPlayer"),
                                                       STR("/Script/UMG.UserWidget:GetOwningLocalPlayer")});
    if (!owningPlayer.Found() || !owningLocalPlayer.Found())
    {
        platform::Log(L"hud players: GetOwningPlayer {} and GetOwningLocalPlayer {} (on Widget or UserWidget); roots not read",
                      owningPlayer.Found() ? L"found" : L"NOT found", owningLocalPlayer.Found() ? L"found" : L"NOT found");
        return 1;
    }
    std::vector<UObject*> widgets;
    UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
    std::map<std::wstring, int> byOwner;
    int                         lines = 0;
    int                         roots = 0;
    for (UObject* widget : widgets)
    {
        if (!IsViewportRoot(widget))
        {
            continue;
        }
        ++roots;
        const auto controller  = owningPlayer.Call(widget);
        const auto localPlayer = owningLocalPlayer.Call(widget);
        const std::wstring owner = std::format(L"{} / {}", controller ? players.Label(*controller) : L"(call failed)",
                                               localPlayer ? players.Label(*localPlayer) : L"(call failed)");
        ++byOwner[owner];
        if (lines < kMaxRootLines)
        {
            platform::Log(L"hud players: root {} '{}': owning player {}", std::wstring(widget->GetClassPrivate()->GetName()),
                          NameOf(widget), owner);
            ++lines;
        }
    }
    for (const auto& [owner, count] : byOwner)
    {
        platform::Log(L"hud players: {} of {} root(s) owned by {}", count, roots, owner);
        ++lines;
    }
    return lines;
}

/// Every FSoftClassPath member of UEndMenuSettings' class default object: name, offset, path.
int LogMenuSettings()
{
    auto* type = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/EndGame.EndMenuSettings"));
    UObject* defaults = type != nullptr ? type->GetClassDefaultObject() : nullptr;
    if (defaults == nullptr)
    {
        platform::Log(L"hud players: /Script/EndGame.EndMenuSettings or its default object not found");
        return 1;
    }
    int lines = 0;
    int named = 0;
    for (FProperty* property :
         RC::Unreal::TFieldRange<FProperty>(type, RC::Unreal::EFieldIterationFlags::IncludeSuper))
    {
        if (!property->IsA<RC::Unreal::FStructProperty>())
        {
            continue;
        }
        auto* structure = static_cast<RC::Unreal::FStructProperty*>(property)->GetStruct().Get();
        if (structure == nullptr || std::wstring(structure->GetName()) != L"SoftClassPath")
        {
            continue;
        }
        // FSoftClassPath begins with its FName AssetPathName (the package.class path).
        alignas(FName) std::uint8_t name[sizeof(FName)] = {};
        if (!CopyGuarded(name, reinterpret_cast<const std::uint8_t*>(defaults) + property->GetOffset_Internal(),
                         sizeof(name)))
        {
            continue;
        }
        const std::wstring text = reinterpret_cast<const FName*>(name)->ToString();
        if (text.empty() || text == L"None")
        {
            continue;
        }
        ++named;
        if (lines < kMaxSettingLines)
        {
            platform::Log(L"hud players: EndMenuSettings {} (+0x{:X}) = {}", std::wstring(property->GetName()),
                          property->GetOffset_Internal(), text);
            ++lines;
        }
    }
    platform::Log(L"hud players: EndMenuSettings names {} widget class(es)", named);
    return lines + 1;
}

}  // namespace

int DumpHudPlayers() noexcept
{
    try
    {
        // Each section logs its own failure and the next still runs.
        const Players players;
        int           lines = LogLocalPlayers(players);
        lines += LogControllerOrder(players);
        lines += LogPrimaryController(players);
        lines += LogRoots(players);
        lines += LogMenuSettings();
        lines += LogHudManagers();
        return lines;
    }
    catch (...)
    {
        platform::Log(L"hud players: failed (an exception while reading)");
        return -1;
    }
}

}  // namespace lss::hud
