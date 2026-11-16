#include "menu/game_ui.h"

#include "facts/menu.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <cstring>
#include <optional>
#include <string>

namespace lss::menu::game_ui {

namespace {

namespace weak = players::weak_object;

// Set at install, then only read.
AssignFn       g_assign    = nullptr;
std::uintptr_t g_ui        = 0;  // the UI system's managers
std::uintptr_t g_topVtable = 0;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto assign    = platform::FindFunction(image, facts::kFStringAssign);
    const auto ui        = players::game_globals::Address(players::game_globals::Global::UiSystem);
    const auto topVtable = image.Rebase(facts::kMainMenuTopWindowVTable, 8);
    if (!assign || !ui || !topVtable || !weak::Known())
    {
        return false;
    }
    g_assign    = reinterpret_cast<AssignFn>(*assign);
    g_ui        = ui;
    g_topVtable = *topVtable;
    return true;
}

AssignFn Assign() noexcept
{
    return g_assign;
}

void* System() noexcept
{
    return reinterpret_cast<void*>(g_ui);
}

void* Manager(std::uint32_t slot) noexcept
{
    return g_ui != 0 && slot < facts::kEndUISystemSlots
               ? platform::ReadPointer(reinterpret_cast<const void*>(g_ui), slot * 8)
               : nullptr;
}

bool IsTopWindow(const void* window) noexcept
{
    return window != nullptr && g_topVtable != 0 && platform::ReadPointer(window, 0) == reinterpret_cast<void*>(g_topVtable);
}

void* WeakAt(const void* object, std::uint32_t offset) noexcept
{
    return object != nullptr ? weak::ObjectAt(static_cast<const std::uint8_t*>(object) + offset) : nullptr;
}

std::wstring ReadString(const void* at) noexcept
try
{
    platform::GameArray s;
    if (!platform::Read(at, s) || s.data == nullptr || s.num <= 1 || s.num > 4096)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(s.num - 1), L'\0');
    return platform::CopyGuarded(text.data(), s.data, text.size() * sizeof(wchar_t)) ? text : std::wstring{};
}
catch (...)
{
    return {};
}

bool AssignString(void* to, const std::wstring& text) noexcept
{
    const platform::GameArray source = platform::SourceString(text);
    void*                     out    = nullptr;
    return g_assign != nullptr && platform::CallGuarded(g_assign, out, to, static_cast<const void*>(&source));
}

bool AssignText(void* to, const wchar_t* text) noexcept
{
    const platform::GameArray source = platform::SourceString(text);
    void*                     out    = nullptr;
    return g_assign != nullptr && platform::CallGuarded(g_assign, out, to, static_cast<const void*>(&source));
}

namespace {

// Square's canvas slot class and its two vector setters, found at the first use (game thread); retried until found.
struct SlotCalls
{
    RC::Unreal::UClass*         cls = nullptr;
    RC::Unreal::UFunction*      setter[2]{};
    std::optional<std::int32_t> at[2];  // the vector's place in each call's parameters
};
SlotCalls g_slot;

bool SlotClassReady()
{
    if (g_slot.cls == nullptr)
    {
        g_slot.cls = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr,
                                                                                       L"/Script/EndGame.EndCanvasPanelSlot");
    }
    return g_slot.cls != nullptr;
}

bool SlotCallsReady(SlotVector which)
{
    constexpr const wchar_t* kFunctions[2] = {L"/Script/EndGame.EndCanvasPanelSlot:SetPosition",
                                              L"/Script/EndGame.EndCanvasPanelSlot:SetSize"};
    constexpr const wchar_t* kParams[2]    = {L"InPosition", L"InSize"};
    const auto               i             = static_cast<std::size_t>(which);
    if (!SlotClassReady())
    {
        return false;
    }
    if (g_slot.setter[i] == nullptr || !g_slot.at[i])
    {
        g_slot.setter[i] = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(nullptr, nullptr, kFunctions[i]);
        g_slot.at[i] = g_slot.setter[i] != nullptr ? players::unreal::ParamOffset(g_slot.setter[i], kParams[i], 8) : std::nullopt;
    }
    return g_slot.cls != nullptr && g_slot.setter[i] != nullptr && g_slot.at[i] && *g_slot.at[i] + 8 <= 16 &&
           g_slot.setter[i]->GetParmsSize() <= 16;
}

bool IsAGuarded(void* object, RC::Unreal::UClass* cls, bool& is) noexcept
{
    __try
    {
        is = static_cast<RC::Unreal::UObject*>(object)->IsA(cls);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

bool IsCanvasSlot(void* slot) noexcept
try
{
    bool is = false;
    return slot != nullptr && SlotClassReady() && IsAGuarded(slot, g_slot.cls, is) && is;
}
catch (...)
{
    return false;
}

bool SetSlotVector(void* slot, SlotVector which, float x, float y) noexcept
try
{
    if (slot == nullptr || !SlotCallsReady(which))
    {
        return false;
    }
    const auto               i = static_cast<std::size_t>(which);
    alignas(16) std::uint8_t frame[16]{};
    std::memcpy(frame + *g_slot.at[i], &x, sizeof x);
    std::memcpy(frame + *g_slot.at[i] + sizeof x, &y, sizeof y);
    return players::unreal::GuardedProcessEvent(static_cast<RC::Unreal::UObject*>(slot), g_slot.setter[i], frame);
}
catch (...)
{
    return false;
}

bool SlotPosition(void* slot, float& x, float& y) noexcept
try
{
    static const auto getter = players::unreal::FindGetter(L"/Script/EndGame.EndCanvasPanelSlot:GetPosition", 8);
    float             at[2]  = {};
    if (slot == nullptr || !getter ||
        !players::unreal::CallGetter(static_cast<RC::Unreal::UObject*>(slot), *getter, at, sizeof at))
    {
        return false;
    }
    x = at[0];
    y = at[1];
    return true;
}
catch (...)
{
    return false;
}

}  // namespace lss::menu::game_ui
