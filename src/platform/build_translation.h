#pragma once

// Game builds, and the translation of the source build's IDA addresses to another build.
//
// Every address in the mod is the IDA address of the source build (game version 1.005). Another build has the code
// elsewhere: the same function sits at another address, and the distances its branches and RIP-relative operands hold
// change with it. For each other build the mod knows, an offline tool (tools/version_match) matched every address the
// mod uses and produced a table of ranges: stretches that are byte for byte the same in both builds except for the
// fields that hold such distances (or absolute pointers into the image), which it lists. Addresses the tool could not
// match and that were re-found by hand carry the bytes the mod must find there instead.
//
// An address missing from a build's table translates to nothing, so the feature using it stands aside, logged,
// exactly as a failed byte check does on an unknown build.

#include "platform/build_value.h"
#include "platform/offset_range.h"

#include <cstdint>
#include <optional>
#include <span>

namespace lss::platform {

/// A stretch of the source build that is the same in another build apart from its moved fields.
struct TranslatedRange
{
    std::uint32_t sourceRva;   // in the source build
    std::uint32_t buildRva;    // in the other build
    std::uint32_t length;
    std::uint32_t firstField;  // index of its first moved field in GameBuild::fields
    std::uint32_t fieldCount;
};

/// A field inside a range whose bytes differ between the builds: a branch or RIP-relative distance, or an
/// absolute pointer into the image. Its offset is from the range's start.
using MovedField = OffsetRange;

/// An address re-found by hand in another build, with that build's bytes there (empty for data: only the address
/// translates). A byte check compares as many of them as the source build's array is long.
struct ReplacedAddress
{
    std::uint32_t                 sourceRva;
    std::uint32_t                 buildRva;
    std::span<const std::uint8_t> bytes;
    std::uint8_t firstInstruction = 0;  // length of the instruction at buildRva if position-independent, else 0
};

struct GameBuild;

/// The hand-found entry for a source RVA in `build`, or nullptr.
const ReplacedAddress* FindReplacedAddress(const GameBuild& build, std::uint32_t sourceRva) noexcept;

struct GameBuild
{
    const wchar_t* name;  // the game version, e.g. L"1.005"
    std::uint32_t  timeDateStamp;
    std::uint32_t  sizeOfImage;
    std::uint32_t  checkSum;
    bool           source;  // the mod's addresses are this build's: they need no translation
    BuildIndex     index;   // which value of each PerBuild constant this build reads (platform/build_value.h)

    std::span<const TranslatedRange> ranges;    // sorted by sourceRva, not overlapping
    std::span<const MovedField>      fields;
    std::span<const ReplacedAddress> replaced;  // sorted by sourceRva; take precedence over ranges
};

/// The known build with these PE header values, or nullptr.
const GameBuild* FindBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage, std::uint32_t checkSum) noexcept;

/// Where the source build's bytes [sourceRva, sourceRva + length) are in `build`, or empty if the build's table does
/// not cover all of them in one range (or one replaced address).
std::optional<std::uint32_t> TranslateRva(const GameBuild& build, std::uint32_t sourceRva, std::uint32_t length) noexcept;

/// The source build's RVA of `build`'s `buildRva`, or empty if no range or replaced address holds it.
std::optional<std::uint32_t> SourceRva(const GameBuild& build, std::uint32_t buildRva) noexcept;

/// True if `live` (read at the translated address) stands for `expected` (the source build's bytes at sourceRva):
/// equal outside the moved fields of the range holding them, or equal to the start of a replaced address's own bytes.
bool MatchesTranslated(const GameBuild& build, std::uint32_t sourceRva, std::span<const std::uint8_t> expected,
                       std::span<const std::uint8_t> live) noexcept;

}  // namespace lss::platform
