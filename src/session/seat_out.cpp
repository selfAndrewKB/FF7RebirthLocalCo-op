#include "session/seat_out.h"

#include "battle/lock_on.h"
#include "camera/camera_override.h"
#include "feedback/ffb_manager_two.h"
#include "feedback/rumble_list_two.h"
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
    feedback::rumble_list_two::PlayerTwoOut(seat);  // stops the seat's channels while it can still be the primary player
    feedback::ffb_manager_two::PlayerTwoOut(seat);  // the seat's component back on the game's force-feedback manager
}

}  // namespace lss::session::seat_out
