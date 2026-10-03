#pragma once

// When a state line is due, for lines whose state has two kinds of parts: steady parts that change on
// events (a layout, a mode, a target) and moving parts that change every frame while something
// animates (a camera's FOV through a zoom, a gauge's height, a button request held for a few frames).
// A steady change is logged at once. A moving change is logged only once it has held still for a set
// number of offers and differs from what was logged, so a zoom logs one line for where it ends
// instead of one per frame, and a value that only flickers is not logged at all.
//
// Offer once per frame (or per pass) from one thread. Steady and Moving need operator== and a default
// value, and must copy without throwing (fixed-size values, no owning containers): Offer is noexcept.
// Give floats a rounded form so a value easing towards its target counts as still.

#include <cstdint>
#include <type_traits>

namespace lss::platform {

template <class Steady, class Moving>
class SettledLog
{
    static_assert(std::is_nothrow_copy_assignable_v<Steady> && std::is_nothrow_copy_assignable_v<Moving>,
                  "SettledLog's parts must copy without throwing");

public:
    /// `stillOffers`: how many offers moving parts must hold still for (at least 1).
    explicit constexpr SettledLog(std::uint32_t stillOffers) noexcept : stillOffers_(stillOffers > 0 ? stillOffers : 1) {}

    /// True when the line is due now: the first offer, a steady change, or moving parts that have held
    /// still for the set number of offers since their last change and differ from those last logged.
    bool Offer(const Steady& steady, const Moving& moving) noexcept
    {
        if (!logged_ || !(steady == loggedSteady_))
        {
            logged_       = true;
            loggedSteady_ = steady;
            loggedMoving_ = moving;
            lastMoving_   = moving;
            heldOffers_   = stillOffers_;
            return true;
        }
        if (!(moving == lastMoving_))
        {
            lastMoving_ = moving;
            heldOffers_ = 0;
            return false;
        }
        if (heldOffers_ < stillOffers_ && ++heldOffers_ == stillOffers_ && !(moving == loggedMoving_))
        {
            loggedMoving_ = moving;
            return true;
        }
        return false;
    }

    /// The next offer is due (as the first).
    void Reset() noexcept { logged_ = false; }

private:
    std::uint32_t stillOffers_;
    std::uint32_t heldOffers_ = 0;
    bool          logged_     = false;
    Steady        loggedSteady_{};
    Moving        loggedMoving_{};
    Moving        lastMoving_{};
};

}  // namespace lss::platform
