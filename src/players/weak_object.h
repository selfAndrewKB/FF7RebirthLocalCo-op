#pragma once

// The engine's weak object pointers, resolved and made by its own functions.
//
// An FWeakObjectPtr is {object index, serial number}, 8 bytes. The seats' modules keep weak pointers
// to the objects they follow (a member, a chocobo, a widget) and read the game's own weak fields;
// both go through FWeakObjectPtr::Get and its assignment (facts/engine.h: kWeakObjectGet,
// kWeakObjectAssign), found here once at startup, before the stages, and called under a guard. A
// weak whose serial is 0 was never set and names nothing. Any thread (the engine's Get reads its
// object array; the callers' objects are theirs to keep alive).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::weak_object {

/// Finds the two functions. False (every resolve answers null, Known false) if their bytes are not what the
/// research read.
bool Install(const platform::GameImage& image) noexcept;

/// Both functions were found: a module that needs weak pointers may install.
bool Known() noexcept;

/// The object the FWeakObjectPtr at `field` names now, by the game's own resolve; null if there is none, it was
/// never set, it is stale, the field is unreadable, or the call faulted.
void* ObjectAt(const void* field) noexcept;

/// The same for a weak's 8 bytes by value.
void* ObjectOf(std::uint64_t weak) noexcept;

/// `object` as a weak value {index, serial}, by the game's own assignment, checked to resolve back to it; 0 if
/// it cannot be (or its serial would be 0).
std::uint64_t WeakOf(void* object) noexcept;

/// Whether `weak` (from WeakOf) still resolves to `object`: false once the object is destroyed.
bool StillNames(std::uint64_t weak, const void* object) noexcept;

/// Writes `object` into the weak at `field` by the game's own assignment (a null object resets the weak to name
/// nothing, as the game's own code clears a weak). False if nothing was found or the call faulted.
bool Assign(std::uint64_t* field, void* object) noexcept;

}  // namespace lss::players::weak_object
