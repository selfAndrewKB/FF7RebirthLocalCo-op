#include "players/unreal_access.h"

#include "platform/engine_addresses.h"
#include "platform/guarded.h"

#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <iterator>
#include <new>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players::unreal {

using RC::Unreal::UClass;
using RC::Unreal::UFunction;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

bool GuardedProcessEvent(UObject* target, UFunction* function, void* params) noexcept
{
    __try
    {
        target->ProcessEvent(function, params);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

namespace {

/// UGameplayStatics::CreatePlayer's parameter frame. Checked against the UFunction's own
/// parameter size before every call, so a different engine layout is refused, not written.
struct CreatePlayerParams
{
    UObject*     WorldContextObject;
    std::int32_t ControllerId;
    bool         bSpawnPlayerController;
    UObject*     ReturnValue;  // the new player's controller: always null without spawning one
};
static_assert(sizeof(CreatePlayerParams) == 24);

using RemoveLocalPlayerFn = bool (*)(UObject* gameInstance, UObject* localPlayer);

// The two engine calls run inside structured-exception guards, so a fault inside the engine is
// reported as a failed call rather than ending the game. FF7R is known to raise one on a
// CreatePlayer path (the login fault that bSpawnPlayerController = true triggers).
//
// The trade-off: a fault part-way through leaves the engine half-changed. That is why every
// caller checks the engine's own list afterwards instead of trusting the call. A stack overflow
// is never caught: the stack is unusable until the guard page is re-armed, so continuing would
// only fault again somewhere less diagnosable.
//
// (platform/guarded.h has the rules for these guards.)

using platform::GuardFilter;

bool GuardedRemove(RemoveLocalPlayerFn remove, UObject* gameInstance, UObject* localPlayer) noexcept
{
    __try
    {
        remove(gameInstance, localPlayer);  // its bool result is always true, so it is not read
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

UObject* GameInstance() noexcept
{
    // FindFirstOf walks every object (≈20–25 ms in this game), so the result is kept: the engine
    // creates the game instance once at start-up and keeps it until exit, across map loads. The kept
    // one is checked alive (its object-array slot still holds it, reachable, same class) before use.
    // Game thread.
    static UObject*     kept      = nullptr;
    static std::int32_t keptIndex = -1;
    static UClass*      keptClass = nullptr;
    if (kept != nullptr)
    {
        auto* item = RC::Unreal::UObjectArray::IndexToObject(keptIndex);
        // `kept` is read (its class) only after its slot shows it live: keep that order if copied.
        if (item != nullptr && item->GetUObject() == kept && !item->IsUnreachable() && !item->IsPendingKill() &&
            kept->GetClassPrivate() == keptClass)
        {
            return kept;
        }
        kept = nullptr;
    }
    UObject* found = UObjectGlobals::FindFirstOf(STR("GameInstance"));  // subclasses match too
    if (found != nullptr)
    {
        kept      = found;
        keptIndex = static_cast<std::int32_t>(found->GetInternalIndex());
        keptClass = found->GetClassPrivate();
    }
    return found;
}

RC::Unreal::TArray<UObject*>* LocalPlayers(UObject* gameInstance) noexcept
{
    if (gameInstance == nullptr)
    {
        return nullptr;
    }
    return gameInstance->GetValuePtrByPropertyNameInChain<RC::Unreal::TArray<UObject*>>(STR("LocalPlayers"));
}

UObject** ControllerField(UObject* localPlayer) noexcept
{
    if (localPlayer == nullptr)
    {
        return nullptr;
    }
    auto* field = localPlayer->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerController"));
    const auto offset = reinterpret_cast<std::uintptr_t>(field) - reinterpret_cast<std::uintptr_t>(localPlayer);
    return field != nullptr && offset == engine::kLocalPlayerControllerOffset ? field : nullptr;
}

namespace {

UObject* ClassDefaultObject(const wchar_t* classPath) noexcept
{
    auto* type = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, classPath);
    return type == nullptr ? nullptr : type->GetClassDefaultObject();
}

/// The offset of `object`'s property `name`, if reflection finds it as one byte at `expected`.
std::optional<std::uint32_t> ByteOffset(UObject* object, const wchar_t* name, std::uint32_t expected) noexcept
{
    if (object == nullptr)
    {
        return std::nullopt;
    }
    auto* property = object->GetPropertyByNameInChain(name);
    if (property == nullptr || property->GetSize() != 1 ||
        property->GetOffset_Internal() != static_cast<std::int32_t>(expected))
    {
        return std::nullopt;
    }
    return expected;
}

std::uint8_t* TwoPlayerSplitscreenLayoutField() noexcept
{
    UObject*   settings = ClassDefaultObject(STR("/Script/EngineSettings.GameMapsSettings"));
    const auto offset   = ByteOffset(settings, STR("TwoPlayerSplitscreenLayout"), engine::kTwoPlayerSplitscreenLayoutOffset);
    return offset ? reinterpret_cast<std::uint8_t*>(settings) + *offset : nullptr;
}

}  // namespace

std::optional<std::uint8_t> ReadTwoPlayerSplitscreenLayout() noexcept
{
    const auto* field = TwoPlayerSplitscreenLayoutField();
    return field == nullptr ? std::nullopt : std::optional<std::uint8_t>(*field);
}

bool WriteTwoPlayerSplitscreenLayout(std::uint8_t value) noexcept
{
    auto* field = TwoPlayerSplitscreenLayoutField();
    if (field == nullptr)
    {
        return false;
    }
    *field = value;  // game thread; the engine reads it on the same thread, in the next redraw
    return true;
}

std::optional<std::uint32_t> AspectRatioAxisConstraintOffset() noexcept
{
    return ByteOffset(ClassDefaultObject(STR("/Script/Engine.LocalPlayer")), STR("AspectRatioAxisConstraint"),
                      engine::kAspectRatioAxisConstraintOffset);
}

CreatedPlayer CreatePlayerWithController(UObject* worldContext, std::int32_t controllerId) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Engine.GameplayStatics:CreatePlayer"));
    auto* statics = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics"));
    if (function == nullptr || statics == nullptr || worldContext == nullptr ||
        function->GetParmsSize() != sizeof(CreatePlayerParams))
    {
        return {};
    }
    UObject* defaultObject = statics->GetClassDefaultObject();
    if (defaultObject == nullptr)
    {
        return {};
    }

    CreatePlayerParams params{};
    params.WorldContextObject     = worldContext;
    params.ControllerId           = controllerId;
    params.bSpawnPlayerController = true;
    const bool called             = GuardedProcessEvent(defaultObject, function, &params);
    return {called, called ? params.ReturnValue : nullptr};
}

std::size_t CountObjects(const wchar_t* className) noexcept
{
    try
    {
        std::vector<UObject*> objects;
        UObjectGlobals::FindAllOf(className, objects);
        return objects.size();
    }
    catch (...)
    {
        return 0;  // only an allocation failure; a count is diagnostic
    }
}

namespace {

/// A reflected UKismetSystemLibrary function and the class default object to call it on.
struct KismetCall
{
    UFunction* function      = nullptr;
    UObject*   defaultObject = nullptr;
};

KismetCall FindKismet(const wchar_t* path) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    auto* library  = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary"));
    if (function == nullptr || library == nullptr)
    {
        return {};
    }
    return {function, library->GetClassDefaultObject()};
}

/// The offset of a parameter in `function`'s frame, if it exists and fits.
std::optional<std::int32_t> ParamOffset(UFunction* function, const wchar_t* name, std::int32_t size) noexcept
{
    auto* property = function->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));
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

/// Calls `call` with a zeroed parameter frame in which `text` is placed as an FString at
/// `stringOffset` and `fill` writes the other inputs; `read` then sees the frame before the
/// string is released.
template <class Fill, class Read>
bool CallWithString(const KismetCall& call, std::int32_t stringOffset, const wchar_t* text, Fill&& fill,
                    Read&& read) noexcept
{
    constexpr std::int32_t kMaxFrame = 64;
    if (call.defaultObject == nullptr || call.function->GetParmsSize() > kMaxFrame)
    {
        return false;
    }
    alignas(16) std::uint8_t frame[kMaxFrame]{};
    auto* string = new (frame + stringOffset) RC::Unreal::FString(text);
    fill(frame);
    const bool called = GuardedProcessEvent(call.defaultObject, call.function, frame);
    if (called)
    {
        read(frame);
    }
    string->~FString();
    return called;
}

void NoInputs(std::uint8_t*) noexcept {}
void NoOutputs(const std::uint8_t*) noexcept {}

}  // namespace

std::optional<std::int32_t> ConsoleVariableInt(const wchar_t* variable) noexcept
{
    const auto call = FindKismet(STR("/Script/Engine.KismetSystemLibrary:GetConsoleVariableIntValue"));
    if (call.function == nullptr)
    {
        return std::nullopt;
    }
    const auto name   = ParamOffset(call.function, STR("VariableName"), sizeof(RC::Unreal::FString));
    const auto result = ParamOffset(call.function, STR("ReturnValue"), sizeof(std::int32_t));
    if (!name || !result)
    {
        return std::nullopt;
    }
    std::optional<std::int32_t> value;
    CallWithString(call, *name, variable, NoInputs, [&](const std::uint8_t* frame) {
        value = *reinterpret_cast<const std::int32_t*>(frame + *result);
    });
    return value;
}

bool ExecuteConsoleCommand(const wchar_t* command) noexcept
{
    const auto call = FindKismet(STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
    UObject*   world = GameInstance();  // the world context; the engine picks player 1's controller
    if (call.function == nullptr || world == nullptr)
    {
        return false;
    }
    const auto context = ParamOffset(call.function, STR("WorldContextObject"), sizeof(UObject*));
    const auto text    = ParamOffset(call.function, STR("Command"), sizeof(RC::Unreal::FString));
    if (!context || !text)
    {
        return false;
    }
    // SpecificPlayer stays null: the engine then uses player 1's controller, as the console does.
    return CallWithString(
        call, *text, command,
        [&](std::uint8_t* frame) { *reinterpret_cast<UObject**>(frame + *context) = world; }, NoOutputs);
}

std::optional<ViewportSize> ViewportSizeOf(UObject* playerController) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Engine.PlayerController:GetViewportSize"));
    if (function == nullptr || playerController == nullptr)
    {
        return std::nullopt;
    }
    const auto sizeX = ParamOffset(function, STR("SizeX"), sizeof(std::int32_t));
    const auto sizeY = ParamOffset(function, STR("SizeY"), sizeof(std::int32_t));
    constexpr std::int32_t kMaxFrame = 16;
    if (!sizeX || !sizeY || function->GetParmsSize() > kMaxFrame)
    {
        return std::nullopt;
    }
    alignas(16) std::uint8_t frame[kMaxFrame]{};
    if (!GuardedProcessEvent(playerController, function, frame))
    {
        return std::nullopt;
    }
    return ViewportSize{*reinterpret_cast<const std::int32_t*>(frame + *sizeX),
                        *reinterpret_cast<const std::int32_t*>(frame + *sizeY)};
}

namespace {

/// A reflected UGameplayStatics function and its class default object, if its world-context
/// parameter and `name` (of `size` bytes) sit where they fit in a frame of at most kMaxFrame bytes.
struct StaticsCall
{
    static constexpr std::int32_t kMaxFrame = 16;

    UFunction*   function      = nullptr;
    UObject*     defaultObject = nullptr;
    std::int32_t context       = -1;
    std::int32_t value         = -1;
};

StaticsCall FindStatics(const wchar_t* path, const wchar_t* name, std::int32_t size) noexcept
{
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    auto* statics  = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics"));
    if (function == nullptr || statics == nullptr || function->GetParmsSize() > StaticsCall::kMaxFrame)
    {
        return {};
    }
    const auto context = ParamOffset(function, STR("WorldContextObject"), sizeof(UObject*));
    const auto value   = ParamOffset(function, name, size);
    UObject*   object  = statics->GetClassDefaultObject();
    if (!context || !value || object == nullptr)
    {
        return {};
    }
    return {function, object, *context, *value};
}

}  // namespace

std::optional<bool> SplitscreenForceDisabled(UObject* worldContext) noexcept
{
    const auto call = FindStatics(STR("/Script/Engine.GameplayStatics:IsSplitscreenForceDisabled"), STR("ReturnValue"), 1);
    if (call.function == nullptr || worldContext == nullptr)
    {
        return std::nullopt;
    }
    alignas(16) std::uint8_t frame[StaticsCall::kMaxFrame]{};
    *reinterpret_cast<UObject**>(frame + call.context) = worldContext;
    if (!GuardedProcessEvent(call.defaultObject, call.function, frame))
    {
        return std::nullopt;
    }
    return frame[call.value] != 0;
}

bool SetForceDisableSplitscreen(UObject* worldContext, bool disable) noexcept
{
    const auto call = FindStatics(STR("/Script/Engine.GameplayStatics:SetForceDisableSplitscreen"), STR("bDisable"), 1);
    if (call.function == nullptr || worldContext == nullptr)
    {
        return false;
    }
    alignas(16) std::uint8_t frame[StaticsCall::kMaxFrame]{};
    *reinterpret_cast<UObject**>(frame + call.context) = worldContext;
    frame[call.value] = disable ? 1 : 0;
    return GuardedProcessEvent(call.defaultObject, call.function, frame);
}

bool RemoveLocalPlayer(UObject* gameInstance, UObject* localPlayer, std::uintptr_t removeLocalPlayer) noexcept
{
    if (gameInstance == nullptr || localPlayer == nullptr)
    {
        return false;
    }
    // The game instance's class decides which vtable is used; RE found no class overriding this
    // entry, and this check makes that a precondition rather than an assumption.
    const auto vtable = *reinterpret_cast<const std::uintptr_t* const*>(gameInstance);
    const auto entry  = vtable[engine::kRemoveLocalPlayerVTableOffset / sizeof(std::uintptr_t)];
    if (entry != removeLocalPlayer)
    {
        return false;
    }
    return GuardedRemove(reinterpret_cast<RemoveLocalPlayerFn>(entry), gameInstance, localPlayer);
}

namespace {

/// The name's text into `out` (truncated). Kept out of line: NameGuarded's __try cannot share a
/// function with the string it builds.
__declspec(noinline) void NameCopy(std::uint64_t name, wchar_t* out, std::size_t capacity)
{
    const std::wstring text = RC::Unreal::FName(static_cast<std::int64_t>(name)).ToString();
    const std::size_t  size = std::min(text.size(), capacity - 1);
    std::copy_n(text.data(), size, out);
    out[size] = L'\0';
}

bool NameGuarded(std::uint64_t name, wchar_t* out, std::size_t capacity) noexcept
{
    __try
    {
        NameCopy(name, out, capacity);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

std::wstring NameText(std::uint64_t name) noexcept
{
    wchar_t    text[128] = {};
    const bool read      = NameGuarded(name, text, std::size(text));
    try
    {
        return read ? std::wstring(text) : std::wstring(L"?");
    }
    catch (...)
    {
        return {};
    }
}

}  // namespace lss::players::unreal
