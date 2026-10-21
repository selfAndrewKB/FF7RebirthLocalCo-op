#pragma once

// One change to the game's code, as every redirect and in-place patch makes it.
//
// The bytes of one instruction (or a call's five) inside one aligned 16-byte block, or two when a call
// site crosses into the next (call_redirect.h, InstallFromOwnThread), compared against what the
// research read, then swapped in one atomic store per block (code_memory.h), and put back the same
// way, only while they are still ours. The three redirects (call_redirect.h, entry_redirect.h,
// code_redirect.h) and the in-place patch (instruction_patch.h) hold their change as one of these; a
// change that is toggled (renderer/submit_hints.h) is applied again from the same bytes.
//
// A change over two blocks is two swaps one after the other: a thread running the bytes between the
// two would take a mix, so the caller applies and reverts such a change only from the one thread that
// runs them, from outside the function holding them. If the second swap refuses, the first is put
// back, and the log says if that fails too (the bytes are then torn).

#include "platform/code_memory.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::platform {

class BlockPatch
{
public:
    /// Changes the `expected.size()` bytes at `site` (live address) to `desired`, of the same length, if they are
    /// `expected` and lie in one aligned 16-byte block (or two, when `crossingAllowed`). False, changing nothing,
    /// if they are not, or a change of this object is applied.
    bool Apply(std::uintptr_t site, std::span<const std::uint8_t> expected, std::span<const std::uint8_t> desired,
               bool crossingAllowed = false) noexcept;

    /// Puts the original bytes back, if this object changed them and they are still ours. False if they were
    /// changed after us: they are then left alone, and the change counts as applied still. True with nothing
    /// applied.
    bool Revert() noexcept;

    /// Applies the last change again after a Revert, if the bytes are the original ones again. False if they are
    /// not, if the change is applied, or if none was ever applied.
    bool Reapply() noexcept;

    bool IsApplied() const noexcept
    {
        return applied_;
    }

    /// The change spans two blocks.
    bool Crosses() const noexcept
    {
        return crosses_;
    }

    /// The aligned block holding the first changed byte (0 before the first Apply).
    std::uintptr_t Block() const noexcept
    {
        return block_;
    }

private:
    bool Swap(const CodeBlock (&from)[2], const CodeBlock (&to)[2]) noexcept;

    std::uintptr_t block_   = 0;
    bool           crosses_ = false;
    bool           applied_ = false;
    CodeBlock      original_[2]{};  // the block(s) as read before the change
    CodeBlock      patched_[2]{};   // the same with the change in
};

}  // namespace lss::platform
