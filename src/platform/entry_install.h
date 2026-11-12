#pragma once

// An entry redirect placed where the build allows: at the function's entry when its first instruction sits inside one
// 16-byte block, else at its second instruction (platform/entry_redirect.h). The same function crosses a block in one
// build and fits in the other, so a module names the two instructions' lengths once and lets this choose.

#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>

namespace lss::platform {

/// Installs `redirect` on `fact`'s function (found by its entry bytes) at its entry, else at its second instruction.
/// `first` and `second` are the two instructions' lengths. The caller has checked that no branch of the game lands on
/// the second instruction and that it is a store the redirect may run before the replacement. False (nothing changed)
/// when the function is not found or neither placement is allowed.
inline bool InstallAtEntryOrSecond(EntryRedirect& redirect, const GameImage& image, const FunctionFact& fact,
                                   std::uintptr_t replacement, std::size_t first = 5, std::size_t second = 5) noexcept
{
    const auto bytes = fact.entryBytes;
    const auto at    = FindFunction(image, fact);
    return at && (redirect.Install(*at, image.EntryInstruction(fact.address, bytes.first(first)), replacement) ||
                  redirect.InstallAtSecondInstruction(*at, bytes.first(first), bytes.subspan(first, second), replacement));
}

}  // namespace lss::platform
