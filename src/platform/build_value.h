#pragma once

// A value that differs between game builds: a field offset or vtable offset that one build moved.
//
// Addresses translate through a table (platform/build_translation.h), but offsets cannot: they are not in the image,
// they are numbers in the mod's own code. A constant that differs between the builds is declared with both values, the
// source build's first, beside the evidence for the other:
//     inline constexpr platform::PerBuild<std::uint32_t> kControllerLeader{0x5C0, 0x5A0};  // 1.004: RE-...md
// and reads as the running build's value wherever the old constant was used as a run-time value. The running build
// is set once at startup, before anything is installed (GameImage::Locate). An unknown build reads the source
// build's values, as every address does.

#include <atomic>
#include <cstdint>

namespace lss::platform {

/// The builds a value can differ in, in release order.
enum class BuildIndex : std::uint8_t
{
    V1004,
    Source,  // 1.005: the build the mod's addresses and offsets are written in
};

namespace detail {
inline std::atomic<BuildIndex> g_build{BuildIndex::Source};
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
    T source;  // the value in the source build, the one the code is written for
    T v1004;

    constexpr T For(BuildIndex build) const noexcept { return build == BuildIndex::V1004 ? v1004 : source; }

    operator T() const noexcept { return For(RunningBuild()); }
};

}  // namespace lss::platform
