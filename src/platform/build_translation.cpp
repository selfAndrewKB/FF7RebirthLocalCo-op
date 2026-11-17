#include "platform/build_translation.h"

#include "platform/builds/build_1_004.h"
#include "platform/builds/build_1_004_replaced.h"

#include <algorithm>
#include <array>

namespace lss::platform {

namespace {

// The source build: Steam build 23447986, game version 1.005 (exe 124,317,952 bytes).
constexpr GameBuild kSource{.name          = L"1.005",
                             .timeDateStamp = 0x6A16CED2,
                             .sizeOfImage   = 0x099D9000,
                             .checkSum      = 0x0769EA6E,
                             .source        = true,
                             .index         = BuildIndex::Source};

// Steam build 20553787, game version 1.004 (exe 126,263,624 bytes). Tables: tools/version_match.
constexpr GameBuild k1004{.name          = L"1.004",
                          .timeDateStamp = 0x68FD6FDE,
                          .sizeOfImage   = 0x09BAD000,
                          .checkSum      = 0x07877C99,
                          .source        = false,
                          .index         = BuildIndex::V1004,
                          .ranges        = builds::v1_004::kRanges,
                          .fields        = builds::v1_004::kFields,
                          .replaced      = builds::v1_004::kReplaced};

constexpr std::array<const GameBuild*, 2> kBuilds{&kSource, &k1004};

const ReplacedAddress* FindReplaced(const GameBuild& build, std::uint32_t sourceRva) noexcept
{
    const auto it = std::lower_bound(build.replaced.begin(), build.replaced.end(), sourceRva,
                                     [](const ReplacedAddress& r, std::uint32_t rva) { return r.sourceRva < rva; });
    return it != build.replaced.end() && it->sourceRva == sourceRva ? &*it : nullptr;
}

/// The range holding [sourceRva, sourceRva + length) whole, or nullptr.
const TranslatedRange* FindRange(const GameBuild& build, std::uint32_t sourceRva, std::uint32_t length) noexcept
{
    // The last range starting at or before sourceRva; ranges do not overlap, so it is the only candidate.
    const auto it = std::upper_bound(build.ranges.begin(), build.ranges.end(), sourceRva,
                                     [](std::uint32_t rva, const TranslatedRange& r) { return rva < r.sourceRva; });
    if (it == build.ranges.begin())
    {
        return nullptr;
    }
    const TranslatedRange& range = *(it - 1);
    const std::uint64_t    end   = static_cast<std::uint64_t>(sourceRva) + length;
    return end <= static_cast<std::uint64_t>(range.sourceRva) + range.length ? &range : nullptr;
}

}  // namespace

const ReplacedAddress* FindReplacedAddress(const GameBuild& build, std::uint32_t sourceRva) noexcept
{
    return build.source ? nullptr : FindReplaced(build, sourceRva);
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

std::optional<std::uint32_t> TranslateRva(const GameBuild& build, std::uint32_t sourceRva, std::uint32_t length) noexcept
{
    if (build.source)
    {
        return sourceRva;
    }
    if (const ReplacedAddress* replaced = FindReplaced(build, sourceRva))
    {
        return replaced->buildRva;
    }
    if (const TranslatedRange* range = FindRange(build, sourceRva, length == 0 ? 1 : length))
    {
        return range->buildRva + (sourceRva - range->sourceRva);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> SourceRva(const GameBuild& build, std::uint32_t buildRva) noexcept
{
    if (build.source)
    {
        return buildRva;
    }
    for (const ReplacedAddress& replaced : build.replaced)
    {
        if (replaced.buildRva == buildRva)
        {
            return replaced.sourceRva;
        }
    }
    // Not sorted by buildRva; a linear pass is fine for the few hundred ranges (callers use it for logs and the odd
    // return-address test, never per frame in bulk).
    for (const TranslatedRange& range : build.ranges)
    {
        if (buildRva >= range.buildRva && buildRva - range.buildRva < range.length)
        {
            return range.sourceRva + (buildRva - range.buildRva);
        }
    }
    return std::nullopt;
}

bool MatchesTranslated(const GameBuild& build, std::uint32_t sourceRva, std::span<const std::uint8_t> expected,
                       std::span<const std::uint8_t> live) noexcept
{
    if (live.size() != expected.size())
    {
        return false;
    }
    if (build.source)
    {
        return std::equal(expected.begin(), expected.end(), live.begin());
    }
    if (const ReplacedAddress* replaced = FindReplaced(build, sourceRva))
    {
        // A hand-found address checks the build's own bytes there (the exe's, recorded when it was re-found), as many
        // as the source build's array is long. Its meaning was confirmed by hand; this proves nothing else has
        // changed them since (another mod, a different exe).
        return replaced->bytes.size() >= live.size() && std::equal(live.begin(), live.end(), replaced->bytes.begin());
    }
    const TranslatedRange* range = FindRange(build, sourceRva, static_cast<std::uint32_t>(expected.size()));
    if (range == nullptr || static_cast<std::uint64_t>(range->firstField) + range->fieldCount > build.fields.size())
    {
        return false;
    }
    const auto          fields = build.fields.subspan(range->firstField, range->fieldCount);
    const std::uint32_t start  = sourceRva - range->sourceRva;  // where `expected` begins within the range
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
