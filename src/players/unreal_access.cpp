#include "players/unreal_access.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "players/object_slot.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>

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

bool SetRootFlag(void* object, bool set) noexcept
{
    __try
    {
        auto* const o = static_cast<UObject*>(object);
        set ? o->SetRootSet() : o->ClearRootSet();
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool ReadRootFlag(void* object, bool& rooted) noexcept
{
    __try
    {
        rooted = static_cast<UObject*>(object)->HasAnyInternalFlags(RC::Unreal::EInternalObjectFlags::RootSet);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

StaticCall FindStatic(const wchar_t* classPath, const wchar_t* functionPath) noexcept
{
    auto*    function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, functionPath);
    auto*    owner    = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, classPath);
    UObject* object   = owner == nullptr ? nullptr : owner->GetClassDefaultObject();
    return function == nullptr || object == nullptr ? StaticCall{} : StaticCall{function, object};
}

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
        // `kept` is read (its class) only after its slot shows it live: keep that order if copied.
        if (LiveInSlot(keptIndex, kept) && kept->GetClassPrivate() == keptClass)
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

namespace {

/// AActor::RootComponent's offset through `actor`'s class, or -1.
std::int32_t RootComponentOffset(UObject* actor) noexcept
{
    try
    {
        auto* property = actor->GetPropertyByNameInChain(STR("RootComponent"));
        return property != nullptr && property->GetSize() == sizeof(void*) ? property->GetOffset_Internal() : -1;
    }
    catch (...)
    {
        return -1;
    }
}

}  // namespace

const std::byte* RootOf(const void* actor) noexcept
{
    // AActor::RootComponent, the same for every actor: every thread that finds it stores the same value.
    static std::atomic<std::int32_t> rootOffset{-1};
    if (actor == nullptr)
    {
        return nullptr;
    }
    // Looked up once; guarded against a structured exception too (a stale actor's class read faults, not throws).
    std::int32_t offset = rootOffset.load(std::memory_order_relaxed);
    if (offset < 0)
    {
        if (!platform::CallGuarded(&RootComponentOffset, offset, static_cast<UObject*>(const_cast<void*>(actor))) || offset < 0)
        {
            return nullptr;
        }
        rootOffset.store(offset, std::memory_order_relaxed);
    }
    const std::byte* root = nullptr;
    return platform::CopyGuarded(&root, static_cast<const std::byte*>(actor) + offset, sizeof(root)) ? root : nullptr;
}

namespace {

// USceneComponent's world transform: its rotation (a quaternion x y z w) and translation.
constexpr std::uint32_t kComponentWorldRotation = 0x1C0;
constexpr std::uint32_t kComponentWorldLocation = 0x1D0;

}  // namespace

bool ActorLocation(const void* actor, float (&out)[3]) noexcept
{
    const std::byte* root = RootOf(actor);
    return root != nullptr && platform::CopyGuarded(out, root + kComponentWorldLocation, sizeof(out));
}

bool ActorYaw(const void* actor, float& yawRadians) noexcept
{
    const std::byte* root = RootOf(actor);
    float            q[4] = {};
    if (root == nullptr || !platform::CopyGuarded(q, root + kComponentWorldRotation, sizeof(q)))
    {
        return false;
    }
    yawRadians = std::atan2(2.0f * (q[3] * q[2] + q[0] * q[1]), 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]));
    return std::isfinite(yawRadians);
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
    return field != nullptr && offset == facts::kLocalPlayerControllerOffset ? field : nullptr;
}

UObject* LocalPlayerController(UObject* localPlayer) noexcept
{
    UObject** field = ControllerField(localPlayer);
    return field != nullptr ? *field : nullptr;
}

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
