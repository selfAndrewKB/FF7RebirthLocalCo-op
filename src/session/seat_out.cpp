#include "session/seat_out.h"

#include "battle/lock_on.h"
#include "camera/camera_override.h"
#include "feedback/seat_ffb_managers.h"
#include "feedback/traversal_rumble.h"
#include "players/seats.h"

namespace lss::session::seat_out {

void LetGo(int seat) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return;
    }
    camera::camera_override::PlayerTwoOut(seat);  // the member's camera delegates back to the engine's, while it is still held
    battle::lock_on::PlayerTwoOut(seat);
    // Stops the seat's channels while it can still be the primary player.
    feedback::traversal_rumble::PlayerTwoOut(seat);
    feedback::seat_ffb_managers::PlayerTwoOut(seat);  // the seat's component back on the game's force-feedback manager
}

}  // namespace lss::session::seat_out
