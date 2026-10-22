#include "players/split_settings_access.h"

#include "facts/engine.h"
#include "players/unreal_access.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace lss::players::split_settings {

namespace {

using RC::Unreal::UClass;
using unreal::StaticCall;
using unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

/// A one-byte field on a class default object, at the offset the engine reads it (facts/engine.h): the one shape
/// of the two layout fields and the axis constraint.
struct ByteSetting
{
    const wchar_t* classPath;
    const wchar_t* name;
    std::uint32_t  offset;
};

constexpr const wchar_t* kGameMapsSettings = STR("/Script/EngineSettings.GameMapsSettings");

constexpr ByteSetting kTwoPlayerLayout{kGameMapsSettings, STR("TwoPlayerSplitscreenLayout"),
                                       facts::kTwoPlayerSplitscreenLayoutOffset};
constexpr ByteSetting kThreePlayerLayout{kGameMapsSettings, STR("ThreePlayerSplitscreenLayout"),
                                         facts::kThreePlayerSplitscreenLayoutOffset};
constexpr ByteSetting kAspectRatioAxisConstraint{STR("/Script/Engine.LocalPlayer"), STR("AspectRatioAxisConstraint"),
                                                 facts::kAspectRatioAxisConstraintOffset};

UObject* ClassDefaultObject(const wchar_t* classPath) noexcept
{
    auto* type = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, classPath);
    return type == nullptr ? nullptr : type->GetClassDefaultObject();
}

/// The setting's field on its class default object, if reflection finds it as one byte at its offset; else null.
std::uint8_t* Field(const ByteSetting& setting) noexcept
{
    UObject* object = ClassDefaultObject(setting.classPath);
    if (object == nullptr)
    {
        return nullptr;
    }
    auto* property = object->GetPropertyByNameInChain(setting.name);
    if (property == nullptr || property->GetSize() != 1 ||
        property->GetOffset_Internal() != static_cast<std::int32_t>(setting.offset))
    {
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t*>(object) + setting.offset;
}

std::optional<std::uint8_t> Read(const ByteSetting& setting) noexcept
{
    const auto* field = Field(setting);
    return field == nullptr ? std::nullopt : std::optional<std::uint8_t>(*field);
}

bool Write(const ByteSetting& setting, std::uint8_t value) noexcept
{
    auto* field = Field(setting);
    if (field == nullptr)
    {
        return false;
    }
    *field = value;  // game thread; the engine reads it on the same thread, in the next redraw
    return true;
}

/// A reflected UGameplayStatics function and its class default object, if its world-context
/// parameter and `name` (of `size` bytes) sit where they fit in a frame of at most kMaxFrame bytes.
struct StaticsCall : StaticCall
{
    static constexpr std::int32_t kMaxFrame = 16;

    std::int32_t context = -1;
    std::int32_t value   = -1;
};

StaticsCall FindStatics(const wchar_t* path, const wchar_t* name, std::int32_t size) noexcept
{
    const StaticCall call = unreal::FindStatic(unreal::kGameplayStatics, path);
    if (call.function == nullptr || call.function->GetParmsSize() > StaticsCall::kMaxFrame)
    {
        return {};
    }
    const auto context = unreal::ParamOffset(call.function, STR("WorldContextObject"), sizeof(UObject*));
    const auto value   = unreal::ParamOffset(call.function, name, size);
    if (!context || !value)
    {
        return {};
    }
    return {call, *context, *value};
}

}  // namespace

std::optional<std::uint8_t> TwoPlayerLayout() noexcept
{
    return Read(kTwoPlayerLayout);
}

bool SetTwoPlayerLayout(std::uint8_t value) noexcept
{
    return Write(kTwoPlayerLayout, value);
}

std::optional<std::uint8_t> ThreePlayerLayout() noexcept
{
    return Read(kThreePlayerLayout);
}

bool SetThreePlayerLayout(std::uint8_t value) noexcept
{
    return Write(kThreePlayerLayout, value);
}

std::optional<std::uint32_t> AspectRatioAxisConstraintOffset() noexcept
{
    const bool found = Field(kAspectRatioAxisConstraint) != nullptr;
    return found ? std::optional<std::uint32_t>(kAspectRatioAxisConstraint.offset) : std::nullopt;
}

std::optional<bool> ForceDisabled(UObject* worldContext) noexcept
{
    const auto call =
        FindStatics(STR("/Script/Engine.GameplayStatics:IsSplitscreenForceDisabled"), STR("ReturnValue"), 1);
    if (call.function == nullptr || worldContext == nullptr)
    {
        return std::nullopt;
    }
    alignas(16) std::uint8_t frame[StaticsCall::kMaxFrame]{};
    *reinterpret_cast<UObject**>(frame + call.context) = worldContext;
    if (!unreal::GuardedProcessEvent(call.defaultObject, call.function, frame))
    {
        return std::nullopt;
    }
    return frame[call.value] != 0;
}

bool SetForceDisabled(UObject* worldContext, bool disable) noexcept
{
    const auto call = FindStatics(STR("/Script/Engine.GameplayStatics:SetForceDisableSplitscreen"), STR("bDisable"), 1);
    if (call.function == nullptr || worldContext == nullptr)
    {
        return false;
    }
    alignas(16) std::uint8_t frame[StaticsCall::kMaxFrame]{};
    *reinterpret_cast<UObject**>(frame + call.context) = worldContext;
    frame[call.value] = disable ? 1 : 0;
    return unreal::GuardedProcessEvent(call.defaultObject, call.function, frame);
}

}  // namespace lss::players::split_settings
