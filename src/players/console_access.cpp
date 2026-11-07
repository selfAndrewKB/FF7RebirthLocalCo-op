#include "players/console_access.h"

#include "players/unreal_access.h"

#if LSS_DEV
#include "platform/command_registry.h"
#include "platform/log.h"
#endif

#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/UFunction.hpp>

#include <cwchar>
#include <iterator>
#include <new>

namespace lss::players::console {

namespace {

using unreal::StaticCall;
using unreal::UObject;

/// A reflected UKismetSystemLibrary function and the class default object to call it on.
StaticCall FindKismet(const wchar_t* path) noexcept
{
    return unreal::FindStatic(unreal::kKismetSystemLibrary, path);
}

void NoInputs(std::uint8_t*) noexcept {}
void NoOutputs(const std::uint8_t*) noexcept {}

/// A console variable's value through `getter` (GetConsoleVariable<T>Value): the one shape of the int and float
/// readers. Empty if the function cannot be found, its frame is not the one expected, or it raised an exception.
template <class T>
std::optional<T> Variable(const wchar_t* getter, const wchar_t* variable) noexcept
{
    const auto call = FindKismet(getter);
    if (call.function == nullptr)
    {
        return std::nullopt;
    }
    const auto name   = unreal::ParamOffset(call.function, STR("VariableName"), sizeof(RC::Unreal::FString));
    const auto result = unreal::ParamOffset(call.function, STR("ReturnValue"), sizeof(T));
    if (!name || !result)
    {
        return std::nullopt;
    }
    std::optional<T> value;
    unreal::CallWithString(call.defaultObject, call.function, *name, variable, NoInputs, [&](const std::uint8_t* frame) {
        value = *reinterpret_cast<const T*>(frame + *result);
    });
    return value;
}

}  // namespace

std::optional<std::int32_t> VariableInt(const wchar_t* variable) noexcept
{
    return Variable<std::int32_t>(STR("/Script/Engine.KismetSystemLibrary:GetConsoleVariableIntValue"), variable);
}

std::optional<float> VariableFloat(const wchar_t* variable) noexcept
{
    return Variable<float>(STR("/Script/Engine.KismetSystemLibrary:GetConsoleVariableFloatValue"), variable);
}

bool SetVariableInt(const wchar_t* variable, std::int32_t value) noexcept
{
    wchar_t command[96]{};
    if (std::swprintf(command, std::size(command), L"%ls %d", variable, value) < 0)
    {
        return false;
    }
    return Execute(command);
}

bool Execute(const wchar_t* command) noexcept
{
    const auto call  = FindKismet(STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
    UObject*   world = unreal::GameInstance();  // the world context; the engine picks player 1's controller
    if (call.function == nullptr || world == nullptr)
    {
        return false;
    }
    const auto context = unreal::ParamOffset(call.function, STR("WorldContextObject"), sizeof(UObject*));
    const auto text    = unreal::ParamOffset(call.function, STR("Command"), sizeof(RC::Unreal::FString));
    if (!context || !text)
    {
        return false;
    }
    // SpecificPlayer stays null: the engine then uses player 1's controller, as the console does.
    return unreal::CallWithString(
        call.defaultObject, call.function, *text, command,
        [&](std::uint8_t* frame) { *reinterpret_cast<UObject**>(frame + *context) = world; }, NoOutputs);
}

#if LSS_DEV

void RegisterCommands(std::function<bool()> anySeatIn)
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"console",
                       .shape = control::Shape::Text,
                       .help  = L"an engine console command, as typed in the console",
                       .set   = [](const Value& v) {
                           const std::wstring& command = std::get<std::wstring>(v);
                           platform::Log(L"control: console command '{}'", command);
                           return Execute(command.c_str()) ? Outcome{}
                                                           : Outcome{false, L"the console command could not be called"};
                       }});
    control::Register(
        {.name = L"quit",
         .help = L"quit the game through the engine (every other player must be out)",
         .set  = [anySeatIn = std::move(anySeatIn)](const Value&) {
             if (anySeatIn())
             {
                 return Outcome{false, L"player 2 or 3 is still in ('leave', 'p3.leave' and their 'wait-until' first)"};
             }
             platform::Log(L"control: quitting the game through the engine's quit command");
             return Execute(L"quit") ? Outcome{} : Outcome{false, L"the quit command could not be called"};
         }});
}

#endif

}  // namespace lss::players::console
