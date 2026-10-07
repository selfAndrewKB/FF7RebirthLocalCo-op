#include "platform/build_translation.h"

#include "platform/builds/build_1_005.h"
#include "platform/builds/build_1_005_replaced.h"

#include <algorithm>
#include <array>

namespace lss::platform {

namespace {

// The research build: Steam build 20553787, game version 1.004 (exe 126,263,624 bytes).
constexpr GameBuild kResearch{.name          = L"1.004",
                               .timeDateStamp = 0x68FD6FDE,
                               .sizeOfImage   = 0x09BAD000,
                               .checkSum      = 0x07877C99,
                               .research      = true,
                               .index         = BuildIndex::Research};

// Steam build 23447986, game version 1.005 (exe 124,317,952 bytes). Tables: tools/version_match.
constexpr GameBuild k1005{.name          = L"1.005",
                          .timeDateStamp = 0x6A16CED2,
                          .sizeOfImage   = 0x099D9000,
                          .checkSum      = 0x0769EA6E,
                          .research      = false,
                          .index         = BuildIndex::V1005,
                          .ranges        = builds::v1_005::kRanges,
                          .fields        = builds::v1_005::kFields,
                          .replaced      = builds::v1_005::kReplaced};

constexpr std::array<const GameBuild*, 2> kBuilds{&kResearch, &k1005};

const ReplacedAddress* FindReplaced(const GameBuild& build, std::uint32_t oldRva) noexcept
{
    const auto it = std::lower_bound(build.replaced.begin(), build.replaced.end(), oldRva,
                                     [](const ReplacedAddress& r, std::uint32_t rva) { return r.oldRva < rva; });
    return it != build.replaced.end() && it->oldRva == oldRva ? &*it : nullptr;
}

/// The range holding [oldRva, oldRva + length) whole, or nullptr.
const TranslatedRange* FindRange(const GameBuild& build, std::uint32_t oldRva, std::uint32_t length) noexcept
{
    // The last range starting at or before oldRva; ranges do not overlap, so it is the only candidate.
    const auto it = std::upper_bound(build.ranges.begin(), build.ranges.end(), oldRva,
                                     [](std::uint32_t rva, const TranslatedRange& r) { return rva < r.oldRva; });
    if (it == build.ranges.begin())
    {
        return nullptr;
    }
    const TranslatedRange& range = *(it - 1);
    const std::uint64_t    end   = static_cast<std::uint64_t>(oldRva) + length;
    return end <= static_cast<std::uint64_t>(range.oldRva) + range.length ? &range : nullptr;
}

}  // namespace

const ReplacedAddress* FindReplacedAddress(const GameBuild& build, std::uint32_t oldRva) noexcept
{
    return build.research ? nullptr : FindReplaced(build, oldRva);
}

const GameBuild* FindBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage, std::uint32_t checkSum) noexcept
{
    for (const GameBuild* build : kBuilds)
    {
        if (build->timeDateStamp == timeDateStamp && build->sizeOfImage == sizeOfImage && build->checkSum == checkSum)
        {
            return build;
        }
    }
    return nullptr;
}

std::optional<std::uint32_t> TranslateRva(const GameBuild& build, std::uint32_t oldRva, std::uint32_t length) noexcept
{
    if (build.research)
    {
        return oldRva;
    }
    if (const ReplacedAddress* replaced = FindReplaced(build, oldRva))
    {
        return replaced->newRva;
    }
    if (const TranslatedRange* range = FindRange(build, oldRva, length == 0 ? 1 : length))
    {
        return range->newRva + (oldRva - range->oldRva);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ResearchRva(const GameBuild& build, std::uint32_t newRva) noexcept
{
    if (build.research)
    {
        return newRva;
    }
    for (const ReplacedAddress& replaced : build.replaced)
    {
        if (replaced.newRva == newRva)
        {
            return replaced.oldRva;
        }
    }
    // Not sorted by newRva; a linear pass is fine for the few hundred ranges (callers use it for logs and the odd
    // return-address test, never per frame in bulk).
    for (const TranslatedRange& range : build.ranges)
    {
        if (newRva >= range.newRva && newRva - range.newRva < range.length)
        {
            return range.oldRva + (newRva - range.newRva);
        }
    }
    return std::nullopt;
}

bool MatchesTranslated(const GameBuild& build, std::uint32_t oldRva, std::span<const std::uint8_t> expected,
                       std::span<const std::uint8_t> live) noexcept
{
    if (live.size() != expected.size())
    {
        return false;
    }
    if (build.research)
    {
        return std::equal(expected.begin(), expected.end(), live.begin());
    }
    if (const ReplacedAddress* replaced = FindReplaced(build, oldRva))
    {
        // A hand-found address checks the build's own bytes there (the exe's, recorded when it was re-found), as many
        // as the research build's array is long. Its meaning was confirmed by hand; this proves nothing else has
        // changed them since (another mod, a different exe).
        return replaced->bytes.size() >= live.size() && std::equal(live.begin(), live.end(), replaced->bytes.begin());
    }
    const TranslatedRange* range = FindRange(build, oldRva, static_cast<std::uint32_t>(expected.size()));
    if (range == nullptr || static_cast<std::uint64_t>(range->firstField) + range->fieldCount > build.fields.size())
    {
        return false;
    }
    const auto     fields = build.fields.subspan(range->firstField, range->fieldCount);
    const std::uint32_t start  = oldRva - range->oldRva;  // where `expected` begins within the range
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const std::uint64_t at    = static_cast<std::uint64_t>(start) + i;
        const bool          moved = std::any_of(fields.begin(), fields.end(), [at](const MovedField& f) {
            return at >= f.offset && at < static_cast<std::uint64_t>(f.offset) + f.size;
        });
        if (!moved && expected[i] != live[i])
        {
            return false;
        }
    }
    return true;
}

}  // namespace lss::platform
