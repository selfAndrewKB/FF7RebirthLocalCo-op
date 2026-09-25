#pragma once

// Reading and writing a native engine object's field at a fixed offset.
//
// Every offset passed here is one read in IDA at the instruction that uses it (listed in
// player_two/engine_facts.h). memcpy keeps the access free of alignment and aliasing assumptions.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::player_two {

template <class T>
T ReadField(const void* object, std::uint32_t offset) noexcept
{
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(T));
    return value;
}

template <class T>
void WriteField(void* object, std::uint32_t offset, T value) noexcept
{
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(T));
}

inline void* ReadPointer(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? nullptr : ReadField<void*>(object, offset);
}

/// An object's C++ vtable (its first 8 bytes). Blueprint classes use their native parent's vtable.
inline std::uintptr_t VTableOf(const void* object) noexcept
{
    return object == nullptr ? 0 : ReadField<std::uintptr_t>(object, 0);
}

}  // namespace lss::player_two
