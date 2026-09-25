#pragma once

// Temporarily replaces one 8-byte engine value for the length of one engine call, and puts it back
// only if nothing else changed it meanwhile.
//
// Used for the party manager's primary-controller field: player 2's updates run with that field
// naming player 2's controller, so the engine code inside them reads and writes player 2's objects
// instead of player 1's. The field is an aligned FWeakObjectPtr {index, serial}, so one atomic
// 8-byte store replaces it whole, and a reader on another thread sees either value, never a mix.
//
// Pure logic on a caller-supplied address: tested on the host (tests/swap_window_test.cpp).

#include <cstdint>

namespace lss::player_two {

class SwapWindow
{
public:
    enum class Opened
    {
        Swapped,      // the slot now holds the replacement; Close puts the original back
        AlreadyHeld,  // the slot already held the replacement (a nested window); Close does nothing
        Changed,      // the slot did not hold the expected value; nothing was written
        Misaligned,   // the slot cannot be written atomically; nothing was written
    };

    SwapWindow() = default;
    SwapWindow(const SwapWindow&)            = delete;
    SwapWindow& operator=(const SwapWindow&) = delete;

    /// Replaces `*slot` with `replacement` if it holds `expected`.
    Opened Open(std::uint64_t* slot, std::uint64_t expected, std::uint64_t replacement) noexcept;

    /// Puts the original back if the slot still holds the replacement. Returns false, writing
    /// nothing, if it does not: someone else wrote the slot inside the window, and their value is
    /// newer than ours. Returns true when there was nothing to restore.
    bool Close() noexcept;

    bool IsOpen() const noexcept { return slot_ != nullptr; }

private:
    std::uint64_t* slot_        = nullptr;
    std::uint64_t  original_    = 0;
    std::uint64_t  replacement_ = 0;
};

}  // namespace lss::player_two
