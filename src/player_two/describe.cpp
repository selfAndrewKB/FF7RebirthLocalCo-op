#include "player_two/describe.h"

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

}  // namespace lss::player_two
