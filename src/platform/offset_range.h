#pragma once

// A stretch of bytes inside a block, by its offset from the block's start and its length. Pure.
//
// The one shape for it in the mod: a field a build moved inside a translated range (platform/build_translation.h), and
// a part of a game object copied per player (the focus context, the field-action manager's ranges).

#include <cstdint>

namespace lss::platform {

struct OffsetRange
{
    std::uint32_t offset;
    std::uint32_t size;
};

}  // namespace lss::platform
