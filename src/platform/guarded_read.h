#pragma once

// The guarded reads of engine memory: platform/guarded.h's CopyGuarded and WriteGuarded under the
// shapes every module reads with. By address or by object and offset; as a bool with the value out
// (Read), or as the value with zero for "unreadable" (ReadAt, ReadField, ReadPointer, ReadPointerAt).
// A null address or object is "not there" without a fault. memcpy keeps the access free of alignment
// and aliasing assumptions. Every offset passed here is one read in IDA at the instruction that uses
// it (the modules' facts).
//
// No Windows.h here: this header reaches every module through player_two/object_fields.h, and
// Windows.h's macros (Yield, GetObject, min/max) must not. The two guards are declared, not included.

#include <cstddef>
#include <cstdint>

namespace lss::platform {

bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept;  // platform/guarded.h
bool WriteGuarded(void* to, const void* from, std::size_t length) noexcept;  // platform/guarded.h

/// The value at `address`. False if the address is null or the read faulted; `out` is then not a value.
template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return address != 0 && CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

/// The same, by pointer.
template <class T>
bool Read(const void* address, T& out) noexcept
{
    return address != nullptr && CopyGuarded(&out, address, sizeof(T));
}

/// The field of `object` at `offset` (one read in IDA at the instruction that uses it). False if the
/// object is null or the read faulted.
template <class T>
bool Read(const void* object, std::size_t offset, T& out) noexcept
{
    return object != nullptr && CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

/// The value at `address`, or T{} (zero) when the address is null or the read faulted: for values whose
/// zero means "not there" (pointers, flags, counts). A field whose zero is a real value is read with Read.
template <class T>
T ReadAt(std::uintptr_t address) noexcept
{
    T value{};
    return Read(address, value) ? value : T{};  // a partial copy is not a value
}

/// The field of `object` at `offset`, or T{} when the object is null or the read faulted.
template <class T>
T ReadField(const void* object, std::size_t offset) noexcept
{
    T value{};
    return Read(object, offset, value) ? value : T{};
}

/// A pointer field of `object` at `offset`; null when the object is null or the read faulted.
inline void* ReadPointer(const void* object, std::size_t offset) noexcept
{
    return ReadField<void*>(object, offset);
}

/// The pointer stored at `address` (a global's slot); null when the address is null or the read faulted.
inline void* ReadPointerAt(std::uintptr_t address) noexcept
{
    return ReadAt<void*>(address);
}

/// Writes `value` at `address`, which may be unmapped. False if the address is null or the write faulted.
template <class T>
bool WriteAt(std::uintptr_t address, const T& value) noexcept
{
    return address != 0 && WriteGuarded(reinterpret_cast<void*>(address), &value, sizeof(T));
}

}  // namespace lss::platform
