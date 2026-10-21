#pragma once

// Pages for the host tests that EXECUTE the bytes the mod writes near the game, or that read memory
// the game might have freed.
//
// ExecutablePage: one 4 KiB page a test fills with a stand-in for the game's code (laid out as the
// real function is around the redirected site), then seals executable and calls. NoAccessPage: a
// page nothing may touch, for the guards. Both are released with the object.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace lss::test {

/// A page of code: write the bytes, seal it, call into it. Unwritten bytes are int3, so running off
/// the end of a stand-in faults at once instead of wandering.
class ExecutablePage
{
public:
    static constexpr std::size_t kSize = 0x1000;

    ExecutablePage() noexcept
        : memory_(static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
    {
        std::memset(memory_, 0xCC, kSize);
    }

    /// The page with `code` at its start, sealed.
    explicit ExecutablePage(std::span<const std::uint8_t> code) noexcept : ExecutablePage()
    {
        Write(0, code);
        Seal();
    }

    ~ExecutablePage()
    {
        VirtualFree(memory_, 0, MEM_RELEASE);
    }

    ExecutablePage(const ExecutablePage&)            = delete;
    ExecutablePage& operator=(const ExecutablePage&) = delete;

    /// Copies `bytes` to `offset`. Before Seal only.
    void Write(std::size_t offset, std::span<const std::uint8_t> bytes) noexcept
    {
        std::memcpy(memory_ + offset, bytes.data(), bytes.size());
    }

    /// Writes the rel32 displacement of a 5-byte call or jump at `at` that reaches `target` (both page offsets).
    void WriteRel32(std::size_t at, std::size_t target) noexcept
    {
        const auto displacement = static_cast<std::int32_t>(static_cast<std::ptrdiff_t>(target) - (at + 5));
        std::memcpy(memory_ + at + 1, &displacement, sizeof(displacement));
    }

    /// Makes the page executable and read-only, and flushes the instruction cache.
    void Seal() noexcept
    {
        DWORD previous = 0;
        VirtualProtect(memory_, kSize, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, kSize);
    }

    std::uintptr_t At(std::size_t offset = 0) const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(memory_) + offset;
    }

    /// The code at `offset` as a function of type `Fn` (a function-pointer type).
    template <class Fn>
    Fn Function(std::size_t offset = 0) const noexcept
    {
        return reinterpret_cast<Fn>(At(offset));
    }

private:
    std::uint8_t* memory_ = nullptr;
};

/// A committed page that no access may touch: every read, write or execute of it faults.
class NoAccessPage
{
public:
    static constexpr std::size_t kSize = 0x1000;

    NoAccessPage() noexcept
        : memory_(static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS)))
    {
    }

    ~NoAccessPage()
    {
        VirtualFree(memory_, 0, MEM_RELEASE);
    }

    NoAccessPage(const NoAccessPage&)            = delete;
    NoAccessPage& operator=(const NoAccessPage&) = delete;

    std::uintptr_t At(std::size_t offset = 0) const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(memory_) + offset;
    }

    template <class T = void>
    T* Pointer(std::size_t offset = 0) const noexcept
    {
        return reinterpret_cast<T*>(At(offset));
    }

private:
    std::uint8_t* memory_ = nullptr;
};

}  // namespace lss::test
