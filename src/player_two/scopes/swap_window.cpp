#include "player_two/scopes/swap_window.h"

#include <atomic>

namespace lss::player_two {

SwapWindow::Opened SwapWindow::Open(std::uint64_t* slot, std::uint64_t expected, std::uint64_t replacement) noexcept
{
    if (slot == nullptr || reinterpret_cast<std::uintptr_t>(slot) % std::atomic_ref<std::uint64_t>::required_alignment != 0)
    {
        return Opened::Misaligned;
    }
    std::atomic_ref<std::uint64_t> value(*slot);
    std::uint64_t                  current = value.load();
    if (current == replacement)
    {
        return Opened::AlreadyHeld;
    }
    if (current != expected || !value.compare_exchange_strong(current, replacement))
    {
        return Opened::Changed;
    }
    slot_        = slot;
    original_    = expected;
    replacement_ = replacement;
    return Opened::Swapped;
}

bool SwapWindow::Close() noexcept
{
    if (slot_ == nullptr)
    {
        return true;
    }
    std::atomic_ref<std::uint64_t> value(*slot_);
    std::uint64_t                  current = replacement_;
    const bool                     restored = value.compare_exchange_strong(current, original_);
    slot_ = nullptr;
    return restored;
}

}  // namespace lss::player_two
