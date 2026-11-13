#pragma once

// A native engine object's fields at fixed offsets, under the mod's names (namespace lss, every module).
//
// The guarded reads are platform/guarded_read.h's, one family for the whole mod; this header brings
// them into lss, where every module reads engine objects, and adds the unguarded write. Every
// offset passed here is one read in IDA at the instruction that uses it (the shared ones in
// facts/). memcpy keeps the access free of alignment and aliasing assumptions.
// Reads are guarded: a read from an object whose memory was released gives zero (null) instead of
// ending the game. Pointers, flags and counts read zero as "not there"; a field whose zero is a real
// value is read with Read, which says whether it was readable. WriteField is not guarded: a write
// into freed memory must not be hidden. WriteAt is guarded and says whether the write landed, for
// the modules that write a game system's fields and act on that.

#include "platform/angles.h"
#include "platform/guarded_read.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss {

using platform::Read;           // bool Read(address, out); bool Read(object, offset, out)
using platform::ReadAt;         // T ReadAt<T>(address)
using platform::ReadField;      // T ReadField<T>(object, offset)
using platform::ReadPointer;    // void* ReadPointer(object, offset)
using platform::ReadPointerAt;  // void* ReadPointerAt(address)
using platform::WriteAt;        // bool WriteAt(address, value)

template <class T>
void WriteField(void* object, std::uint32_t offset, T value) noexcept
{
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(T));
}

/// An object's C++ vtable (its first 8 bytes). Blueprint classes use their native parent's vtable.
inline std::uintptr_t VTableOf(const void* object) noexcept
{
    return ReadField<std::uintptr_t>(object, 0);
}

/// The yaw (degrees) of the FQuat {x, y, z, w} field at `offset` of `object` (platform::QuaternionYawDegrees): the
/// direction a transform or an edge faces. An unreadable field reads as the identity rotation, yaw 0.
inline float QuatFieldYaw(const void* object, std::uint32_t offset) noexcept
{
    struct Quat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };
    Quat read, q;
    if (Read(object, offset, read))
    {
        q = read;  // a copy that faulted part-way is no value: the identity stands
    }
    return platform::QuaternionYawDegrees(q.x, q.y, q.z, q.w);
}

}  // namespace lss
