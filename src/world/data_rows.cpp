#include "world/data_rows.h"

#include "platform/guarded.h"
#include "players/object_fields.h"
#include "world/field_rules.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>

namespace lss::world::data_rows {

namespace {

constexpr std::uint32_t kAccessorRow = 0x08;

}  // namespace

const std::byte* MappedData(const std::byte* field) noexcept
{
    return field_rules::MappedPointer(field, ReadField<std::uint64_t>(field, 0));
}

std::int32_t MappedCount(const std::byte* field, std::int32_t max) noexcept
{
    const auto count = ReadField<std::int32_t>(field, 8);
    return count > 0 ? std::min(count, max) : 0;
}

std::wstring MappedText(const std::byte* element, std::int32_t cap)
{
    const auto  count = ReadField<std::int32_t>(element, 8);
    const auto* data  = MappedData(element);
    if (data == nullptr || count < 2 || count > cap + 1)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(count - 1), L'\0');
    if (!platform::CopyGuarded(text.data(), data, text.size() * sizeof(wchar_t)))
    {
        return {};
    }
    if (!field_rules::IsRowName(text))
    {
        return {};  // row names are ASCII: anything else means the offset is wrong
    }
    return text;
}

std::vector<std::uint64_t> MappedNames(const std::byte* field, std::int32_t max)
{
    std::vector<std::uint64_t> names;
    const auto* data  = MappedData(field);
    const auto  count = MappedCount(field, max);
    for (std::int32_t i = 0; i < count && data != nullptr; ++i)
    {
        const auto name = ReadField<std::uint64_t>(data, 8 * static_cast<std::uint32_t>(i));
        if (name == 0)
        {
            break;
        }
        names.push_back(name);
    }
    return names;
}

std::uint64_t MappedNameAt(const std::byte* field, std::int32_t index) noexcept
{
    const auto* data = MappedData(field);
    if (data == nullptr || index < 0 || index >= ReadField<std::int32_t>(field, 8))
    {
        return 0;
    }
    return ReadField<std::uint64_t>(data, 8 * static_cast<std::uint32_t>(index));
}

const std::byte* Row(AccessorFn accessor, std::uint64_t name)
{
    if (accessor == nullptr || name == 0)
    {
        return nullptr;
    }
    alignas(16) std::array<std::byte, 64> buffer{};
    void* filled = nullptr;
    if (!platform::CallGuarded(accessor, filled, static_cast<void*>(buffer.data()), name))
    {
        return nullptr;
    }
    return static_cast<const std::byte*>(ReadPointer(buffer.data(), kAccessorRow));
}

std::uint64_t FindName(const std::wstring& text) noexcept
{
    try
    {
        const RC::Unreal::FName fname(text.c_str(), RC::Unreal::FNAME_Find);
        return fname.GetComparisonIndex() | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fname.GetNumber())) << 32);
    }
    catch (...)
    {
        return 0;
    }
}

const std::byte* Row(AccessorFn accessor, const std::wstring& name)
{
    const std::uint64_t value = FindName(name);
    return value != 0 ? Row(accessor, value) : nullptr;
}

}  // namespace lss::world::data_rows
