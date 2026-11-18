#pragma once

// Rewrites one instruction of the game's code in place, with one of the same length, and puts it back.
//
// For a fix that is a different operand, not different code (renderer/submit_hints.h: a vtable 8 bytes
// on; the submission census's tagged hint vtables), a redirect's jump and code page would only add
// moving parts. The rules are the redirects' (block_patch.h): the replaced bytes are compared against
// what the research read before anything is written; they lie inside one aligned 16-byte block, swapped
// in a single atomic store; the replacement has the original's length (it may be more than one instruction), so
// the original instruction's start and end stay boundaries and a thread stopped anywhere in the block resumes on an
// instruction boundary either way. A
// patch that is toggled goes in again from the same bytes (Reapply). The destructor leaves an applied
// patch in place, as the redirects do; only Restore() removes it.

#include "platform/block_patch.h"

#include <cstdint>
#include <span>

namespace lss::platform {

class InstructionPatch
{
public:
    InstructionPatch() = default;
    ~InstructionPatch() = default;  // leaves an applied patch in place (see above)

    InstructionPatch(const InstructionPatch&)            = delete;
    InstructionPatch& operator=(const InstructionPatch&) = delete;

    /// Replaces `original` at `site` (live address; one instruction, inside one aligned 16-byte block) with
    /// `replacement` of the same length. Returns false, changing nothing, if any condition fails or a patch of
    /// this object is applied.
    bool Apply(std::uintptr_t site, std::span<const std::uint8_t> original,
               std::span<const std::uint8_t> replacement) noexcept
    {
        return patch_.Apply(site, original, replacement);
    }

    /// Puts the original instruction back, if this object replaced it and it is still ours. Returns false if it
    /// was changed after us (it is then left alone). True with nothing applied.
    bool Restore() noexcept
    {
        return patch_.Revert();
    }

    /// Applies the last patch again after a Restore, if the instruction is the original again. False if it is
    /// not, if the patch is applied, or if none was ever applied.
    bool Reapply() noexcept
    {
        return patch_.Reapply();
    }

    bool IsApplied() const noexcept
    {
        return patch_.IsApplied();
    }

    /// A patch of this object was applied at some time: Reapply has bytes to go in again with.
    bool WasApplied() const noexcept
    {
        return patch_.Block() != 0;
    }

private:
    BlockPatch patch_;
};

}  // namespace lss::platform
