#pragma once

// A value that differs between game builds: a field offset or vtable offset that a later build moved.
//
// Addresses translate through a table (platform/build_translation.h), but offsets cannot: they are not in the image,
// they are numbers in the mod's own code. A constant a later build moved is declared with both values, the research
// build's first, beside the evidence for the other:
//     inline constexpr platform::PerBuild<std::uint32_t> kControllerLeader{0x5A0, 0x5C0};  // 1.005: RE-...md
// and reads as the running build's value wherever the old constant was used as a run-time value. The running build
// is set once at startup, before anything is installed (GameImage::Locate). An unknown build reads the research
// build's values, as every address does.

#include <atomic>
#include <cstdint>

namespace lss::platform {

/// The builds a value can differ in, in release order.
enum class BuildIndex : std::uint8_t
{
    Research,  // 1.004
    V1005,
};

namespace detail {
inline std::atomic<BuildIndex> g_build{BuildIndex::Research};
}

/// Sets the running build. Called by GameImage::Locate, before any feature reads a PerBuild value.
inline void SetRunningBuild(BuildIndex build) noexcept
{
    detail::g_build.store(build, std::memory_order_relaxed);
}

inline BuildIndex RunningBuild() noexcept
{
    return detail::g_build.load(std::memory_order_relaxed);
}

template <class T>
struct PerBuild
{
    T research;  // the value the research documents cite
    T v1005;

    constexpr T For(BuildIndex build) const noexcept { return build == BuildIndex::V1005 ? v1005 : research; }

    operator T() const noexcept { return For(RunningBuild()); }
};

}  // namespace lss::platform
