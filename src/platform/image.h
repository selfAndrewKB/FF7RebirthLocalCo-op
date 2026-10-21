#pragma once

// The game's executable image in memory, and the translation from IDA addresses to live ones.
//
// Every engine address in this mod is written as it appears in IDA (image base 0x140000000) in the research build,
// because that is how the research documents cite them. The game is loaded at a different base each run (ASLR), so
// each address is rebased once, here, and checked to lie inside the image. On a later game build the address is
// first translated through that build's table (platform/build_translation.h).

#include "platform/build_translation.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lss::platform {

class GameImage
{
public:
    /// Finds the running game image. Empty if the process is not FF7 Rebirth.
    static std::optional<GameImage> Locate() noexcept;

    /// An image at `base` of `size` bytes with no build table (addresses as the research build's): a stand-in for
    /// the host tests of code that keeps an image (platform/deferred_sites.h).
    static GameImage At(std::uintptr_t base, std::size_t size) noexcept
    {
        return GameImage(base, size, nullptr);
    }

    /// The live address of a research-build IDA address, or empty if it falls outside the image or this build's
    /// table does not hold all of [idaAddress, idaAddress + length).
    std::optional<std::uintptr_t> Rebase(std::uintptr_t idaAddress, std::size_t length = 1) const noexcept;

    /// True if the bytes at an IDA address are `expected`. This is how the mod confirms that the game build is
    /// the one the research was done on before relying on any address. On a translated build, the fields the
    /// build's table lists as moved (branch and RIP-relative distances) are not compared.
    bool BytesMatch(std::uintptr_t idaAddress, std::span<const std::uint8_t> expected) const noexcept;

    /// The address the RIP-relative instruction at `idaSite` reaches: its `opcode` bytes, then a 32-bit
    /// displacement that ends the instruction, counted from the instruction's end. Empty if the site is outside
    /// the image, its bytes are not `opcode` (BytesMatch, this build's table included) or unreadable.
    std::optional<std::uintptr_t> RipTarget(std::uintptr_t idaSite, std::span<const std::uint8_t> opcode) const noexcept;

    /// The pointer stored at an IDA address (for vtable entries), or empty if outside the image.
    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t idaAddress) const noexcept;

    /// A live address inside the image as the research build's IDA address, for logs and for comparing return
    /// addresses with research addresses. Where this build's table holds no such address, the build's own IDA
    /// address (live - base + 0x140000000).
    std::uintptr_t ResearchAddress(std::uintptr_t live) const noexcept;

    /// The instruction an entry redirect (platform/entry_redirect.h) replaces at a research address, for this build:
    /// `researchFirst` itself on the research build and wherever this build's table matched the bytes; at an address
    /// re-found by hand, this build's own first instruction if it is position-independent and exactly as long as
    /// `researchFirst` (the redirect copies one whole instruction into its trampoline), else empty, which the redirect
    /// refuses. Call after BytesMatch has confirmed the address.
    std::span<const std::uint8_t> EntryInstruction(std::uintptr_t idaAddress,
                                                   std::span<const std::uint8_t> researchFirst) const noexcept;

    std::uintptr_t Base() const noexcept { return base_; }

    /// The game build, or nullptr if it is none the mod knows (addresses are then used untranslated).
    const GameBuild* Build() const noexcept { return build_; }

private:
    GameImage(std::uintptr_t base, std::size_t size, const GameBuild* build) noexcept
        : base_(base), size_(size), build_(build)
    {
    }

    /// True if addresses go through a translation table (a known build other than the research build).
    bool Translating() const noexcept { return build_ != nullptr && !build_->research; }

    std::uintptr_t   base_;
    std::size_t      size_;
    const GameBuild* build_;
};

}  // namespace lss::platform
