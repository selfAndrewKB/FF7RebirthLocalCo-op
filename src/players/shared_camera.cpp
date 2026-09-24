#include "players/shared_camera.h"

namespace lss::players {

bool SharedCamera::Attach(UObject** playerOneController, UObject** playerTwoController) noexcept
{
    if (IsAttached() || playerOneController == nullptr || playerTwoController == nullptr ||
        *playerOneController == nullptr)
    {
        return false;
    }
    if (*playerTwoController != nullptr)
    {
        // Something already gave player 2 a controller; it is not ours to replace.
        return false;
    }

    // Local-player objects live for the whole session, so the field addresses are fixed and
    // kept rather than looked up again on every tick.
    *playerTwoController = *playerOneController;
    oneField_            = playerOneController;
    twoField_            = playerTwoController;
    lent_                = *playerOneController;
    return true;
}

bool SharedCamera::Refresh() noexcept
{
    if (!IsAttached())
    {
        return false;
    }
    const bool holdsLoan = *twoField_ == lent_;
    const bool emptied   = *twoField_ == nullptr;  // the engine cleared player 2's field
    if ((!holdsLoan && !emptied) || *twoField_ == *oneField_)
    {
        // Either the engine gave player 2 a controller of its own (not ours to replace), or the
        // loan is already current.
        return false;
    }
    // Player 1's controller was replaced or cleared, or player 2's field was emptied. Follow
    // player 1, so player 2 never holds a controller the engine has destroyed and does not lose
    // its view.
    *twoField_ = *oneField_;
    lent_      = *oneField_;
    return true;
}

void SharedCamera::Detach() noexcept
{
    if (!IsAttached())
    {
        return;
    }
    // Clear the field if it holds our loan or player 1's current controller (the loan may be a
    // tick behind a controller change). Afterwards it cannot hold player 1's controller. A
    // controller of player 2's own may stay: removing player 2 destroys it, which is correct for a
    // controller that belongs to player 2.
    if (*twoField_ == lent_ || *twoField_ == *oneField_)
    {
        *twoField_ = nullptr;
    }
    oneField_ = twoField_ = nullptr;
    lent_     = nullptr;
}

}  // namespace lss::players
