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
    // Always empty the field. Player 2 never has a controller of its own in this design (it is
    // created without one), so whatever the field holds is borrowed, possibly a stale loan, and
    // RemoveLocalPlayer would destroy it.
    *twoField_ = nullptr;
    Abandon();
}

void SharedCamera::Abandon() noexcept
{
    oneField_ = twoField_ = nullptr;
    lent_     = nullptr;
}

}  // namespace lss::players
