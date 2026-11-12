#include "hud/widget_dump.h"

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
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/command_controller.h"
#include "facts/hud.h"
#include "platform/build_value.h"
#include "platform/command_registry.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_slot.h"
#include "players/unreal_access.h"
#include "scopes/primary_controller.h"

#include <algorithm>
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

constexpr int         kListedWidgets = 60;  // a listing's size, largest first
constexpr std::size_t kFrame    = 256;  // parameter frame; every function used here needs far less

using players::unreal::GuardedProcessEvent;
using players::unreal::ParamOffset;
using platform::CopyGuarded;

// ---- dump widgets, dump slots ------------------------------------------------------------------

/// A reflected UWidget function with no inputs, and where its return value sits in the frame.
struct Getter
{
    UFunction*   function = nullptr;
    std::int32_t offset   = -1;
    std::int32_t size     = 0;
};

std::optional<Getter> FindGetter(const wchar_t* path, std::int32_t minSize) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    if (function == nullptr || function->GetParmsSize() > static_cast<std::int32_t>(kFrame))
    {
        return std::nullopt;
    }
    auto* property = function->FindProperty(RC::Unreal::FName(STR("ReturnValue"), RC::Unreal::FNAME_Find));
    if (property == nullptr || property->GetSize() < minSize || property->GetOffset_Internal() < 0 ||
        property->GetOffset_Internal() + property->GetSize() > function->GetParmsSize())
    {
        return std::nullopt;
    }
    return Getter{function, property->GetOffset_Internal(), property->GetSize()};
}

/// Calls `getter` on `widget` and copies `length` bytes of its return value into `out`.
bool Call(UObject* widget, const Getter& getter, void* out, std::size_t length) noexcept
{
    alignas(16) std::uint8_t frame[kFrame]{};
    if (length > static_cast<std::size_t>(getter.size) || !GuardedProcessEvent(widget, getter.function, frame))
    {
        return false;
    }
    std::memcpy(out, frame + getter.offset, length);
    return true;
}

struct Row
{
    float        area = 0.0f;
    std::wstring line;
};

constexpr int         kMaxSlotDepth = 4;
constexpr int         kMaxSlotLines = 80;
constexpr std::int32_t kAnchorDataSize = 40;  // FAnchorData: FMargin Offsets, FAnchors Anchors, FVector2D Alignment

std::wstring Describe(UObject* object)
{
    if (object == nullptr)
    {
        return L"(none)";
    }
    return std::wstring(object->GetClassPrivate()->GetName()) + L" '" + std::wstring(object->GetName()) + L"'";
}

/// A reflected object-pointer property of `object`, or null.
UObject* ObjectProperty(UObject* object, const wchar_t* name)
{
    auto* property = object->GetPropertyByNameInChain(name);
    if (property == nullptr || property->GetSize() != static_cast<std::int32_t>(sizeof(void*)))
    {
        return nullptr;
    }
    return *object->GetValuePtrByPropertyNameInChain<UObject*>(name);
}

void WalkSlots(UObject* widget, int depth, int& lines)
{
    if (widget == nullptr || depth > kMaxSlotDepth || lines >= kMaxSlotLines)
    {
        return;
    }
    // A user widget: continue into its own tree.
    if (UObject* tree = ObjectProperty(widget, STR("WidgetTree")))
    {
        WalkSlots(ObjectProperty(tree, STR("RootWidget")), depth + 1, lines);
        return;
    }
    // A panel: every slot, then its content.
    if (widget->GetPropertyByNameInChain(STR("Slots")) == nullptr)
    {
        return;
    }
    auto* slots = widget->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("Slots"));
    for (std::int32_t i = 0; slots != nullptr && i < slots->Num() && lines < kMaxSlotLines; ++i)
    {
        UObject* slot    = (*slots)[i];
        UObject* content = slot != nullptr ? ObjectProperty(slot, STR("Content")) : nullptr;
        std::wstring layout;
        if (slot != nullptr)
        {
            auto* data = slot->GetPropertyByNameInChain(STR("LayoutData"));
            if (data != nullptr && data->GetSize() == kAnchorDataSize)
            {
                float v[10] = {};
                std::memcpy(v, slot->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("LayoutData")), sizeof(v));
                const auto* autoSize = slot->GetPropertyByNameInChain(STR("bAutoSize")) != nullptr
                                           ? slot->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("bAutoSize"))
                                           : nullptr;
                layout = std::format(L" anchors ({:.2f},{:.2f})-({:.2f},{:.2f}) offsets ({:.1f},{:.1f},{:.1f},{:.1f}) "
                                     L"alignment ({:.2f},{:.2f}) autosize {}",
                                     v[4], v[5], v[6], v[7], v[0], v[1], v[2], v[3], v[8], v[9],
                                     autoSize != nullptr ? (*autoSize & 1) : -1);
            }
        }
        platform::Log(L"dump slots: {}{} -> {}{}", std::wstring(static_cast<std::size_t>(depth) * 2, L' '),
                      slot != nullptr ? std::wstring(slot->GetClassPrivate()->GetName()) : L"(null slot)",
                      Describe(content), layout);
        ++lines;
        WalkSlots(content, depth + 1, lines);
    }
}

/// Where a user widget is: visible or not, its cached geometry, and its viewport layout (anchors,
/// offsets, alignment; facts/hud.h).
void LogPlacement(UObject* widget)
{
    static const auto isVisible = FindGetter(STR("/Script/UMG.Widget:IsVisible"), 1);
    static const auto geometry  = FindGetter(STR("/Script/UMG.Widget:GetCachedGeometry"), 20);
    bool  visible = false;
    float geom[5] = {};
    if (!isVisible || !geometry || !Call(widget, *isVisible, &visible, 1) || !Call(widget, *geometry, geom, sizeof(geom)))
    {
        return;
    }
    float v[10] = {};
    static_assert(sizeof(v) == facts::kUserWidgetViewportLayoutEnd - facts::kUserWidgetViewportAnchors);
    std::memcpy(v, reinterpret_cast<const std::uint8_t*>(widget) + facts::kUserWidgetViewportAnchors, sizeof(v));
    platform::Log(L"dump slots:   visible {}; geometry size {:.0f}x{:.0f} scale {:.3f} at ({:.0f}, {:.0f}); viewport "
                  L"anchors ({:.2f},{:.2f})-({:.2f},{:.2f}) offsets ({:.0f},{:.0f},{:.0f},{:.0f}) alignment ({:.2f},{:.2f})",
                  visible, geom[0], geom[1], geom[2], geom[3], geom[4], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                  v[8], v[9]);
}

// ---- dump hud-players --------------------------------------------------------------------------

constexpr int kMaxRootLines    = 60;
constexpr int kMaxSettingLines = 400;
constexpr int kMaxControllers  = 4;

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
        for (int p = 0; p < players::kMaxPlayers; ++p)
        {
            controllers_[p] = static_cast<UObject*>(players::generation::ControllerOf(p));
        }
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
        for (int p = 0; p < players::kMaxPlayers; ++p)
        {
            if (object == controllers_[p])
            {
                return std::format(L"{}'s controller", players::SeatName(p));
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
    UObject*              controllers_[players::kMaxPlayers] = {};  // by player: 0 player 1's, then each seat's
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
    if (!scopes::primary_controller::Installed())
    {
        platform::Log(L"hud players: primary controller not readable (the party manager was not found at startup)");
        return 1;
    }
    platform::Log(L"hud players: the party manager's primary controller: {}",
                  players.Label(static_cast<UObject*>(scopes::primary_controller::Current())));
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
    // This class's properties, then each super's (the loader's chain walk would go on to the metaclass's).
    for (RC::Unreal::UStruct* owner = type; owner != nullptr; owner = owner->GetSuperStruct())
    for (FProperty* property : owner->ForEachProperty())
    {
        if (!property->IsA<RC::Unreal::FStructProperty>())
        {
            continue;
        }
        auto* structure = static_cast<RC::Unreal::FStructProperty*>(property)->GetStruct();
        if (structure == nullptr || std::wstring(structure->GetName()) != L"SoftClassPath")
        {
            continue;
        }
        // FSoftClassPath begins with its FName AssetPathName (the package.class path).
        std::uint64_t name = 0;
        if (!CopyGuarded(&name, reinterpret_cast<const std::uint8_t*>(defaults) + property->GetOffset_Internal(),
                         sizeof(name)))
        {
            continue;
        }
        const std::wstring text = players::unreal::NameText(name);  // guarded: "?" if the name pool faulted
        if (text.empty() || text == L"None" || text == L"?")
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

// ---- the HUD's owner: EndUISystem and the stock player layers (research/RE-p2-hud.md) -------------

// EndUISystem (g_EndUISystem) and the members the research read: 8-byte heap pointers, weak object
// pointers as {int32 index, int32 serial} (serial 0 = null), and one-byte flags.
constexpr std::uint32_t  kCompassHolder    = 0x348;  // in the compass manager; its +0 is the weak widget
using facts::kUISetupDeferred;
using facts::kCmdMenuWidget;
using facts::kCmdFieldMenu;
constexpr std::uint32_t  kCtrlStoredController  = 0x558;
using facts::kCmdSetupPending;

// SGameLayerManager::GetPlayerWidgetHostGeometry: IGameLayerManager vtable slot 2,
// FGeometry* (IGameLayerManager*, FGeometry* out, ULocalPlayer*). The interface pointer is held
// by the game viewport client at +0x120 (a weak pointer; its reference controller at +0x128).
constexpr std::uint8_t   kPlayerHostGeometryPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                          0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlayerHostGeometry{L"GameLayerManager player-host geometry", 0x1451803CC,
                                                     kPlayerHostGeometryPrologue};
constexpr std::uint32_t  kViewportLayerManager           = 0x120;
constexpr std::uint32_t  kViewportLayerManagerController = 0x128;
constexpr std::size_t    kGeometryBytes                  = 0x38;  // Size +0, Scale +8, translation +0x2C

using GeometryFn = void* (*)(void* layerManager, void* out, void* localPlayer);

std::optional<std::uintptr_t> PointerAt(std::uintptr_t address) noexcept
{
    std::uintptr_t value = 0;
    return platform::Read(address, value) ? std::optional(value) : std::nullopt;
}

std::optional<std::uint8_t> ByteAt(std::uintptr_t address) noexcept
{
    std::uint8_t value = 0;
    return platform::Read(address, value) ? std::optional(value) : std::nullopt;
}

bool CallGeometry(GeometryFn function, void* layerManager, void* out, void* localPlayer) noexcept
{
    __try
    {
        function(layerManager, out, localPlayer);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// A weak object pointer at `address`, resolved through GUObjectArray with its serial checked.
std::wstring DescribeWeak(std::uintptr_t address)
{
    std::int32_t weak[2] = {};
    if (address == 0 || !CopyGuarded(weak, reinterpret_cast<const void*>(address), sizeof(weak)))
    {
        return L"(unreadable)";
    }
    if (weak[1] == 0)
    {
        return L"(null)";
    }
    // The slot's object must be the slot's own (the walk moves past an unreachable slot) with the weak pointer's
    // serial.
    UObject* object = players::ObjectInSlot(weak[0]);
    if (object == nullptr || object->GetInternalIndex() != weak[0] ||
        players::SlotSerial(object->GetObjectItem()) != weak[1])
    {
        return std::format(L"(stale: index {}, serial {})", weak[0], weak[1]);
    }
    const int          seat = object == players::generation::PlayerOneController()
                                  ? 0
                                  : players::generation::SeatOfController(object);
    const std::wstring who  = seat == 0 || players::SeatIndex(seat)
                                  ? std::format(L" = {}'s controller", players::SeatName(seat))
                                  : std::wstring();
    return std::format(L"{} '{}'{}", std::wstring(object->GetClassPrivate()->GetName()), std::wstring(object->GetName()), who);
}

std::wstring Flag(std::uintptr_t address)
{
    const auto value = ByteAt(address);
    return value ? std::to_wstring(*value) : std::wstring(L"(unreadable)");
}

int LogPlayerLayers(std::uintptr_t playerHostGeometry)
{
    UObject* engine   = UObjectGlobals::FindFirstOf(STR("GameEngine"));
    auto*    viewport = engine != nullptr && engine->GetPropertyByNameInChain(STR("GameViewport")) != nullptr
                            ? *engine->GetValuePtrByPropertyNameInChain<UObject*>(STR("GameViewport"))
                            : nullptr;
    const auto base     = reinterpret_cast<std::uintptr_t>(viewport);
    const auto manager  = viewport != nullptr ? PointerAt(base + kViewportLayerManager) : std::nullopt;
    const auto refCount = viewport != nullptr ? PointerAt(base + kViewportLayerManagerController) : std::nullopt;
    if (playerHostGeometry == 0 || !manager || *manager == 0 || !refCount || *refCount == 0)
    {
        platform::Log(L"hud managers: the game layer manager is not reachable; player layers not read");
        return 1;
    }
    const auto vtable = PointerAt(*manager);
    const auto slot2  = vtable ? PointerAt(*vtable + 2 * sizeof(void*)) : std::nullopt;
    if (!slot2 || *slot2 != playerHostGeometry)
    {
        platform::Log(L"hud managers: the layer manager's vtable slot 2 is not GetPlayerWidgetHostGeometry; not called");
        return 1;
    }
    int lines = 0;
    auto* list = players::unreal::LocalPlayers(players::unreal::GameInstance());
    for (std::int32_t i = 0; list != nullptr && i < list->Num(); ++i)
    {
        alignas(16) std::uint8_t geometry[kGeometryBytes]{};
        if (!CallGeometry(reinterpret_cast<GeometryFn>(*slot2), reinterpret_cast<void*>(*manager), geometry, (*list)[i]))
        {
            platform::Log(L"hud managers: GetPlayerWidgetHostGeometry raised an exception for LocalPlayers[{}]", i);
            ++lines;
            continue;
        }
        float size[2] = {}, scale = 0.0f, translation[2] = {};
        std::memcpy(size, geometry, sizeof(size));
        std::memcpy(&scale, geometry + 8, sizeof(scale));
        std::memcpy(translation, geometry + 0x2C, sizeof(translation));
        platform::Log(L"hud managers: LocalPlayers[{}]'s stock player layer: size {:.0f}x{:.0f}, scale {:.3f}, at ({:.0f}, {:.0f})",
                      i, size[0], size[1], scale, translation[0], translation[1]);
        ++lines;
    }
    return lines;
}

/// The HUD's one owner, EndUISystem: its field command controller's weak widgets and stored controller, the compass
/// holder's widget, the two setup flags, and the stock per-player layer geometry each local player would get.
int LogHudManagers(const platform::GameImage& image)
{
    const auto system = image.Rebase(facts::kEndUISystem, kUISetupDeferred + 1);
    if (!system)
    {
        platform::Log(L"hud managers: EndUISystem not located");
        return 1;
    }
    const auto ctrl    = PointerAt(*system + facts::kUICommandController).value_or(0);
    const auto compass = PointerAt(*system + facts::kUIAreaMap).value_or(0);
    const auto holder  = compass != 0 ? PointerAt(compass + kCompassHolder).value_or(0) : 0;
    platform::Log(L"hud managers: EndUISystem field-command setup pending {}; field command controller {}",
                  Flag(*system + kUISetupDeferred), ctrl != 0 ? L"present" : L"absent");
    int lines = 1;
    if (ctrl != 0)
    {
        platform::Log(L"hud managers: field command: command menu {}; field menu {}; stored controller {}; setup trigger {}",
                      DescribeWeak(ctrl + kCmdMenuWidget), DescribeWeak(ctrl + kCmdFieldMenu),
                      DescribeWeak(ctrl + kCtrlStoredController), Flag(ctrl + kCmdSetupPending));
        ++lines;
    }
    platform::Log(L"hud managers: compass holder's widget {}",
                  holder != 0 ? DescribeWeak(holder) : std::wstring(L"(no holder)"));
    ++lines;
    const std::uintptr_t geometry = platform::FindFunction(image, kPlayerHostGeometry).value_or(0);
    return lines + LogPlayerLayers(geometry);
}

}  // namespace

int DumpSlots(const std::wstring& className) noexcept
{
    try
    {
        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
        int lines = 0;
        int found = 0;
        for (UObject* widget : widgets)
        {
            if (std::wstring(widget->GetClassPrivate()->GetName()) != className)
            {
                continue;
            }
            ++found;
            UObject* tree = ObjectProperty(widget, STR("WidgetTree"));
            UObject* slot = ObjectProperty(widget, STR("Slot"));  // null for a widget on the viewport itself
            std::wstring outers;
            for (UObject* outer = widget->GetOuterPrivate(); outer != nullptr && outers.size() < 400;
                 outer = outer->GetOuterPrivate())
            {
                outers += L" < " + Describe(outer);
            }
            platform::Log(L"dump slots: {} (root {}); its slot {} in {}; outers{}", Describe(widget),
                          Describe(tree != nullptr ? ObjectProperty(tree, STR("RootWidget")) : nullptr), Describe(slot),
                          Describe(slot != nullptr ? ObjectProperty(slot, STR("Parent")) : nullptr), outers);
            LogPlacement(widget);
            WalkSlots(widget, 0, lines);
        }
        if (found == 0)
        {
            platform::Log(L"dump slots: no live widget of class {}", className);
        }
        return lines;
    }
    catch (...)
    {
        platform::Log(L"dump slots: failed (an exception while walking)");
        return -1;
    }
}

int DumpWidgets() noexcept
{
    try
    {
        const auto isVisible = FindGetter(STR("/Script/UMG.Widget:IsVisible"), 1);
        const auto desired   = FindGetter(STR("/Script/UMG.Widget:GetDesiredSize"), 8);
        const auto geometry  = FindGetter(STR("/Script/UMG.Widget:GetCachedGeometry"), 20);
        if (!isVisible || !desired || !geometry)
        {
            platform::Log(L"dump widgets: UWidget's IsVisible, GetDesiredSize or GetCachedGeometry not found");
            return -1;
        }

        std::vector<UObject*> widgets;
        UObjectGlobals::FindAllOf(STR("UserWidget"), widgets);
        std::vector<Row> rows;
        for (UObject* widget : widgets)
        {
            bool visible = false;
            if (!Call(widget, *isVisible, &visible, 1) || !visible)
            {
                continue;
            }
            float want[2] = {};
            // FGeometry begins Size, Scale, AbsolutePosition (UE4 Layout/Geometry.h); read as floats.
            float geom[5] = {};
            if (!Call(widget, *desired, want, sizeof(want)) || !Call(widget, *geometry, geom, sizeof(geom)))
            {
                continue;
            }
            if (geom[0] <= 0.0f || geom[1] <= 0.0f)
            {
                continue;  // never arranged (not on screen)
            }
            Row row;
            row.area = geom[0] * geom[1];
            row.line = std::format(L"dump widgets: {} '{}': desired {:.0f}x{:.0f}; geometry size {:.0f}x{:.0f} scale {:.3f} "
                                   L"at ({:.0f}, {:.0f})",
                                   std::wstring(widget->GetClassPrivate()->GetName()), std::wstring(widget->GetName()),
                                   want[0], want[1], geom[0], geom[1], geom[2], geom[3], geom[4]);
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.area > b.area; });
        platform::Log(L"dump widgets: {} user widgets, {} visible and arranged{}", widgets.size(), rows.size(),
                      rows.size() > kListedWidgets ? L" (largest listed)" : L"");
        const int count = static_cast<int>(std::min<std::size_t>(rows.size(), kListedWidgets));
        for (int i = 0; i < count; ++i)
        {
            platform::Log(L"{}", rows[static_cast<std::size_t>(i)].line);
        }
        return count;
    }
    catch (...)
    {
        platform::Log(L"dump widgets: failed (an exception while listing)");
        return -1;
    }
}

int DumpHudPlayers(const platform::GameImage& image) noexcept
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
        lines += LogHudManagers(image);
        return lines;
    }
    catch (...)
    {
        platform::Log(L"hud players: failed (an exception while reading)");
        return -1;
    }
}

void RegisterDumpCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register(
        {.name  = L"dump",
         .shape = control::Shape::Word,
         .words = {L"widgets", L"hud-players"},
         .help  = L"log each visible HUD widget's desired size and geometry (widgets), or which player the HUD roots "
                  L"and its global player reads name (hud-players)",
         .set   = [](const Value& v) {
             const bool widgets = std::get<std::wstring>(v) == L"widgets";
             int        listed  = -1;
             if (widgets)
             {
                 listed = DumpWidgets();
             }
             else if (const auto image = platform::GameImage::Locate())
             {
                 listed = DumpHudPlayers(*image);
             }
             if (listed < 0)
             {
                 return Outcome{false, L"nothing dumped (see the log)"};
             }
             return Outcome{true, std::format(L"{} {} listed in the log", listed, widgets ? L"widgets" : L"lines")};
         }});
    control::Register({.name  = L"dump-slots",
                       .shape = control::Shape::Text,
                       .help  = L"log a widget class's viewport layout and slots",
                       .set   = [](const Value& v) {
                           const int listed = DumpSlots(std::get<std::wstring>(v));
                           return listed >= 0 ? Outcome{true, std::format(L"{} slots listed in the log", listed)}
                                              : Outcome{false, L"nothing dumped (see the log)"};
                       }});
}

}  // namespace lss::hud
