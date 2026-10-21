#pragma once

// The game's single slots answered for one player for the length of a call.
//
// Square Enix's code keeps one copy of state that is really per player: the party manager's leader and its
// type, the UI system's command controller and key guide, the input object's focus context and merged sticks,
// the battle system's target fields. The mod makes the game's own calls for player 2 with player 2's values in
// those slots and the game's values put back after. A ByteContext is one player's values for a set of live byte
// ranges; a ByteScope writes them in on construction and puts the game's back on destruction. Three kinds of
// range:
//   - Set: a fixed value (a pointer, a type byte). It is written in; the saved live bytes go back out, unless
//     the game wrote the slot inside the scope, in which case the game's newer value is kept (Close reports it).
//   - Exchange: the player's own copy of state the game keeps there (a focus path, a lock). The copy is
//     exchanged with the live bytes both ways, so what the game wrote inside the scope stays the player's.
//   - Keep: nothing of the player's. The live bytes are saved and put back whatever the game wrote inside (a
//     global the call must leave as it found it); Close reports a write.
// The storage is the caller's, sized at compile time (ByteStore<ranges, bytes>): the scopes are built per call
// on the game thread and allocate nothing. A store kept between calls carries an Exchange copy; Retarget moves
// a range to the slot's object as read before each call. No engine dependency: the host test exercises it on
// plain memory. The reads and writes are guarded against a fault (an unreadable slot opens nothing). Not
// thread-safe; one thread per context.
//
// Ported from the Remake mod's platform/byte_scope.h (2026-10-21); the Keep kind, the fixed store and Retarget
// are this mod's additions.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::platform {

class ByteContext
{
public:
    enum class Kind : std::uint8_t
    {
        Set,
        Exchange,
        Keep,
    };

    struct Range
    {
        std::uintptr_t live = 0;
        std::uint32_t  size = 0;
        Kind           kind = Kind::Set;
    };

    ByteContext(const ByteContext&)            = delete;
    ByteContext& operator=(const ByteContext&) = delete;

    /// Replaces the ranges; every value starts zero. Refused (false, unchanged) while open, for a zero address
    /// or size, or past the store's capacity.
    bool Define(std::span<const Range> ranges) noexcept;
    /// Forgets the ranges (an undefined context opens nothing). Refused while open.
    bool Clear() noexcept;
    /// Moves range `index` to another live address, its value kept. Refused while open, out of range or for 0.
    bool Retarget(std::size_t index, std::uintptr_t live) noexcept;

    bool         Defined() const noexcept { return count_ != 0; }
    bool         IsOpen() const noexcept { return open_; }
    std::size_t  Count() const noexcept { return count_; }
    const Range& RangeAt(std::size_t index) const noexcept { return ranges_[index]; }

    /// The value written in (Set), the player's copy (Exchange) or nothing (Keep: what the game left there,
    /// after Close), to initialise or read. Empty if out of range.
    std::span<std::uint8_t>       Value(std::size_t index) noexcept;
    std::span<const std::uint8_t> Value(std::size_t index) const noexcept;

    /// Saves every range and writes the Set and Exchange values in. False (nothing left changed) if already
    /// open, undefined, or a range faulted.
    bool Open() noexcept;
    /// Puts the live bytes back. Returns the number of Set and Keep ranges the game had changed inside (a Set
    /// range is left as the game wrote it, a Keep range put back); 0 when everything was still the scope's own.
    unsigned Close() noexcept;

protected:
    ByteContext() noexcept = default;
    /// The store's arrays (ByteStore gives its own once they exist).
    void Attach(std::span<Range> ranges, std::span<std::uint8_t> values, std::span<std::uint8_t> saved) noexcept;

private:
    std::size_t OffsetOf(std::size_t index) const noexcept;

    std::span<Range>        ranges_;
    std::span<std::uint8_t> values_;
    std::span<std::uint8_t> saved_;
    std::size_t             count_ = 0;
    std::size_t             bytes_ = 0;  // of every range together
    bool                    open_  = false;
};

/// A context with room for `Ranges` ranges of `Bytes` bytes together.
template <std::size_t Ranges, std::size_t Bytes>
class ByteStore final : public ByteContext
{
public:
    ByteStore() noexcept { Attach(ranges_, values_, saved_); }

private:
    std::array<Range, Ranges>       ranges_{};
    std::array<std::uint8_t, Bytes> values_{};
    std::array<std::uint8_t, Bytes> saved_{};
};

class ByteScope
{
public:
    explicit ByteScope(ByteContext& context) noexcept : context_(context), held_(context.Open()) {}
    ~ByteScope() { Release(); }
    ByteScope(const ByteScope&)            = delete;
    ByteScope& operator=(const ByteScope&) = delete;

    bool Held() const noexcept { return held_; }

    /// Closes now (Close's count); the destructor then does nothing. 0 if not held.
    unsigned Release() noexcept
    {
        if (!held_)
        {
            return 0;
        }
        held_    = false;
        changed_ = context_.Close();
        return changed_;
    }

    /// Close's count, after Release or destruction.
    unsigned Changed() const noexcept { return changed_; }

private:
    ByteContext& context_;
    bool         held_    = false;
    unsigned     changed_ = 0;
};

}  // namespace lss::platform
