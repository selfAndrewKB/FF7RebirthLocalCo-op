#pragma once

// Game builds, and the translation of the research build's IDA addresses to another build.
//
// Every address in the mod is the IDA address of the build the research was done on (the research build,
// game version 1.004). A later build moves the code: the same function sits elsewhere, and the distances its
// branches and RIP-relative operands hold change with it. For each known later build, an offline tool
// (tools/version_match) matched every address the mod uses and generated a table of ranges: stretches that
// are byte for byte the same in both builds except for the fields that hold such distances (or absolute
// pointers into the image), which it lists. Addresses the tool could not match and that were re-found by hand
// carry the bytes the mod must find there instead.
//
// An address missing from a build's table translates to nothing, so the feature using it stands aside, logged,
// exactly as a failed byte check does on an unknown build.

#include "platform/build_value.h"
#include "platform/offset_range.h"

#include <cstdint>
#include <optional>
#include <span>

namespace lss::platform {

/// A stretch of the research build that is the same in another build apart from its moved fields.
struct TranslatedRange
{
    std::uint32_t oldRva;      // in the research build
    std::uint32_t newRva;
    std::uint32_t length;
    std::uint32_t firstField;  // index of its first moved field in GameBuild::fields
    std::uint32_t fieldCount;
};

/// A field inside a range whose bytes differ between the builds: a branch or RIP-relative distance, or an
/// absolute pointer into the image. Its offset is from the range's start.
using MovedField = OffsetRange;

/// An address re-found by hand in another build, with that build's bytes there (empty for data: only the address
/// translates). A byte check compares as many of them as the research build's array is long.
struct ReplacedAddress
{
    std::uint32_t                 oldRva;
    std::uint32_t                 newRva;
    std::span<const std::uint8_t> bytes;
    std::uint8_t firstInstruction = 0;  // length of the instruction at newRva if position-independent, else 0
};

struct GameBuild;

/// The hand-found entry for a research RVA in `build`, or nullptr.
const ReplacedAddress* FindReplacedAddress(const GameBuild& build, std::uint32_t oldRva) noexcept;

struct GameBuild
{
    const wchar_t* name;  // the game version, e.g. L"1.005"
    std::uint32_t  timeDateStamp;
    std::uint32_t  sizeOfImage;
    std::uint32_t  checkSum;
    bool           research;  // the addresses were read in this build: they need no translation
    BuildIndex     index;     // which value of each PerBuild constant this build reads (platform/build_value.h)

    std::span<const TranslatedRange> ranges;    // sorted by oldRva, not overlapping
    std::span<const MovedField>      fields;
    std::span<const ReplacedAddress> replaced;  // sorted by oldRva; take precedence over ranges
};

/// The known build with these PE header values, or nullptr.
const GameBuild* FindBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage, std::uint32_t checkSum) noexcept;

/// Where the research build's bytes [oldRva, oldRva + length) are in `build`, or empty if the build's table does
/// not cover all of them in one range (or one replaced address).
std::optional<std::uint32_t> TranslateRva(const GameBuild& build, std::uint32_t oldRva, std::uint32_t length) noexcept;

/// The research build's RVA of `build`'s `newRva`, or empty if no range or replaced address holds it.
std::optional<std::uint32_t> ResearchRva(const GameBuild& build, std::uint32_t newRva) noexcept;

/// True if `live` (read at the translated address) stands for `expected` (the research build's bytes at oldRva):
/// equal outside the moved fields of the range holding them, or equal to the start of a replaced address's own bytes.
bool MatchesTranslated(const GameBuild& build, std::uint32_t oldRva, std::span<const std::uint8_t> expected,
                       std::span<const std::uint8_t> live) noexcept;

}  // namespace lss::platform
