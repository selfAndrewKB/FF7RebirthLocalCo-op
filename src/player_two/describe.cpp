#include "player_two/describe.h"

#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <Unreal/UObject.hpp>

namespace lss::player_two {

std::wstring NameOf(const void* object) noexcept
{
    if (object == nullptr)
    {
        return L"(none)";
    }
    try
    {
        return static_cast<const RC::Unreal::UObject*>(object)->GetName();
    }
    catch (...)
    {
        return L"(unnamed)";
    }
}

std::wstring Who(const void* object) noexcept
{
    if (object == nullptr)
    {
        return L"none";
    }
    const void* const playerOne = generation::PlayerOneController();
    if (object == playerOne)
    {
        return L"player 1's controller";
    }
    if (object == generation::PlayerTwoController())
    {
        return L"player 2's controller";
    }
    if (object == generation::Member())
    {
        return L"player 2's member";
    }
    if (object == ReadPointer(playerOne, facts::kControllerPawn))
    {
        return L"player 1's character";
    }
    return NameOf(object);
}

}  // namespace lss::player_two
